#include "ludus/chess/match.hpp"

#include "ludus/core/binary.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace ludus::chess {
namespace {

constexpr std::string_view archive_magic{"ludus.chess.match"};
constexpr std::uint32_t archive_version = ChessMatchArchive::current_version;

Diagnostic match_error(std::string message) {
    return Diagnostic{DiagnosticCode::validation_failed, std::move(message), {}};
}

Diagnostic archive_error(std::string message) {
    return Diagnostic{DiagnosticCode::serialization_error, std::move(message), {}};
}

Color opposite(Color color) noexcept {
    return color == Color::white ? Color::black : Color::white;
}

MatchOutcome win_for(Color color) noexcept {
    return color == Color::white ? MatchOutcome::white_wins : MatchOutcome::black_wins;
}

std::string repetition_key(const Position& position) {
    std::istringstream input{position.to_fen()};
    std::array<std::string, 4U> fields;
    for (auto& field : fields) {
        input >> field;
    }
    const bool legal_en_passant = std::ranges::any_of(
        position.legal_moves(), [](ChessMove move) {
            return has_flag(move.flags, MoveFlag::en_passant);
        });
    if (!legal_en_passant) {
        fields[3] = "-";
    }
    return fields[0] + ' ' + fields[1] + ' ' + fields[2] + ' ' + fields[3];
}

struct Material {
    int pawns{0};
    int knights{0};
    int bishops{0};
    int rooks{0};
    int queens{0};
    bool light_bishop{false};
    bool dark_bishop{false};
};

std::array<Material, 2U> material(const Position& position) noexcept {
    std::array<Material, 2U> result;
    for (std::uint8_t square = 0U; square < 64U; ++square) {
        const auto piece = position.piece_at(square);
        if (piece.empty() || piece.type == PieceType::king) {
            continue;
        }
        auto& side = result[piece.color == Color::white ? 0U : 1U];
        switch (piece.type) {
        case PieceType::pawn:
            ++side.pawns;
            break;
        case PieceType::knight:
            ++side.knights;
            break;
        case PieceType::bishop:
            ++side.bishops;
            if (((square % 8U) + (square / 8U)) % 2U == 0U) {
                side.dark_bishop = true;
            } else {
                side.light_bishop = true;
            }
            break;
        case PieceType::rook:
            ++side.rooks;
            break;
        case PieceType::queen:
            ++side.queens;
            break;
        case PieceType::none:
        case PieceType::king:
            break;
        }
    }
    return result;
}

bool is_manual_result(MatchResultReason reason) noexcept {
    return reason == MatchResultReason::resignation ||
           reason == MatchResultReason::timeout ||
           reason == MatchResultReason::agreed_draw ||
           reason == MatchResultReason::threefold_repetition ||
           reason == MatchResultReason::fifty_move_rule;
}

void write_optional_time(BinaryWriter& writer, std::optional<std::int64_t> value) {
    writer.boolean(value.has_value());
    if (value) {
        writer.i64(*value);
    }
}

std::optional<std::int64_t> read_optional_time(BinaryReader& reader) {
    return reader.boolean() ? std::optional<std::int64_t>{reader.i64()} : std::nullopt;
}

void write_move(BinaryWriter& writer, ChessMove move) {
    writer.u8(move.from);
    writer.u8(move.to);
    writer.u8(static_cast<std::uint8_t>(move.promotion));
    writer.u8(static_cast<std::uint8_t>(move.flags));
}

ChessMove read_move(BinaryReader& reader) {
    return ChessMove{reader.u8(), reader.u8(),
                     static_cast<PieceType>(reader.u8()),
                     static_cast<MoveFlag>(reader.u8())};
}

} // namespace

bool has_possible_mating_material(const Position& position, Color color) noexcept {
    const auto pieces = material(position);
    const auto& own = pieces[color == Color::white ? 0U : 1U];
    const auto& enemy = pieces[color == Color::white ? 1U : 0U];
    if (own.pawns != 0 || own.rooks != 0 || own.queens != 0 ||
        (own.bishops != 0 && own.knights != 0) ||
        (own.light_bishop && own.dark_bishop)) {
        return true;
    }
    const auto enemy_non_king = enemy.pawns + enemy.knights + enemy.bishops +
                                enemy.rooks + enemy.queens;
    if (own.knights != 0 || own.bishops != 0) {
        return enemy_non_king != 0;
    }
    return false;
}

bool insufficient_material(const Position& position) noexcept {
    const auto pieces = material(position);
    for (const auto& side : pieces) {
        if (side.pawns != 0 || side.rooks != 0 || side.queens != 0 ||
            (side.bishops != 0 && side.knights != 0) || side.knights >= 2) {
            return false;
        }
    }
    const auto total_knights = pieces[0].knights + pieces[1].knights;
    const auto total_bishops = pieces[0].bishops + pieces[1].bishops;
    if (total_knights + total_bishops <= 1) {
        return true;
    }
    if (total_knights != 0) {
        return false;
    }
    const bool any_light = pieces[0].light_bishop || pieces[1].light_bishop;
    const bool any_dark = pieces[0].dark_bishop || pieces[1].dark_bishop;
    return !(any_light && any_dark);
}

ChessMatch::ChessMatch(ChessMatchSettings settings, ChessGame game)
    : settings_(std::move(settings)), game_(std::move(game)) {
    if (settings_.time_control.base_milliseconds) {
        white_remaining_ = *settings_.time_control.base_milliseconds;
        black_remaining_ = *settings_.time_control.base_milliseconds;
    }
    refresh_automatic_result();
}

std::expected<ChessMatch, Diagnostic>
ChessMatch::create(PythonRuntime& runtime, ChessMatchSettings settings) {
    if (settings.white_name.empty() || settings.white_name.size() > 64U ||
        settings.black_name.empty() || settings.black_name.size() > 64U) {
        return std::unexpected(match_error("player names must contain 1..64 bytes"));
    }
    const auto& control = settings.time_control;
    if ((control.base_milliseconds &&
         (*control.base_milliseconds <= 0 ||
          *control.base_milliseconds > maximum_base_milliseconds)) ||
        control.increment_milliseconds < 0 ||
        control.increment_milliseconds > maximum_increment_milliseconds ||
        (!control.base_milliseconds && control.increment_milliseconds != 0)) {
        return std::unexpected(match_error("time control is outside supported bounds"));
    }
    auto game = ChessGame::create(runtime, settings.initial_position);
    if (!game) {
        return std::unexpected(game.error());
    }
    return ChessMatch{std::move(settings), std::move(*game)};
}

std::optional<std::int64_t> ChessMatch::remaining(Color color) const noexcept {
    return color == Color::white ? white_remaining_ : black_remaining_;
}

std::expected<void, Diagnostic> ChessMatch::validate_elapsed(std::int64_t value) const {
    if (value < 0 || value > maximum_base_milliseconds) {
        return std::unexpected(match_error("elapsed clock time is outside supported bounds"));
    }
    return {};
}

std::expected<MatchUpdate, Diagnostic>
ChessMatch::submit(ChessMove move, std::int64_t elapsed_milliseconds) {
    if (result_.terminal()) {
        return std::unexpected(match_error("the chess match has already ended"));
    }
    if (history_cursor_ >= maximum_plies) {
        return std::unexpected(match_error("the chess match exceeds the 20000-ply limit"));
    }
    if (auto valid = validate_elapsed(elapsed_milliseconds); !valid) {
        return std::unexpected(valid.error());
    }
    const auto position = game_.position();
    if (!position) {
        return std::unexpected(position.error());
    }
    const auto moving = position->side_to_move();
    auto& clock = moving == Color::white ? white_remaining_ : black_remaining_;
    const auto clock_before = clock;
    if (clock) {
        if (elapsed_milliseconds >= *clock) {
            *clock = 0;
            const auto opponent = opposite(moving);
            undone_manual_result_.reset();
            undone_manual_elapsed_milliseconds_.reset();
            terminal_elapsed_milliseconds_ = elapsed_milliseconds;
            result_ = ChessMatchResult{
                has_possible_mating_material(*position, opponent) ? win_for(opponent)
                                                                  : MatchOutcome::draw,
                MatchResultReason::timeout, moving, std::nullopt};
            return MatchUpdate{std::nullopt, result_};
        }
        *clock -= elapsed_milliseconds;
    }
    auto committed = game_.submit(move);
    if (!committed) {
        clock = clock_before;
        return std::unexpected(committed.error());
    }
    if (clock) {
        if (*clock > std::numeric_limits<std::int64_t>::max() -
                         settings_.time_control.increment_milliseconds) {
            return std::unexpected(match_error("the chess clock overflowed"));
        }
        *clock += settings_.time_control.increment_milliseconds;
    }
    if (history_cursor_ < history_.size()) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_cursor_),
                       history_.end());
    }
    const auto canonical = game_.move_history().back();
    history_.push_back(TimedPly{canonical, elapsed_milliseconds,
                                white_remaining_, black_remaining_});
    history_cursor_ = history_.size();
    undone_manual_result_.reset();
    undone_manual_elapsed_milliseconds_.reset();
    draw_offer_.reset();
    refresh_automatic_result();
    return MatchUpdate{std::move(*committed), result_};
}

std::expected<MatchUpdate, Diagnostic>
ChessMatch::submit_uci(std::string_view uci, std::int64_t elapsed_milliseconds) {
    const auto position = game_.position();
    if (!position) {
        return std::unexpected(position.error());
    }
    const auto move = position->find_legal_move(uci);
    if (!move) {
        return std::unexpected(move.error());
    }
    return submit(*move, elapsed_milliseconds);
}

std::expected<MatchUpdate, Diagnostic>
ChessMatch::flag(Color color, std::int64_t elapsed_milliseconds) {
    if (result_.terminal()) {
        return std::unexpected(match_error("the chess match has already ended"));
    }
    if (auto valid = validate_elapsed(elapsed_milliseconds); !valid) {
        return std::unexpected(valid.error());
    }
    const auto position = game_.position();
    if (!position) {
        return std::unexpected(position.error());
    }
    if (position->side_to_move() != color) {
        return std::unexpected(match_error("only the active player's clock may flag"));
    }
    auto& clock = color == Color::white ? white_remaining_ : black_remaining_;
    if (!clock) {
        return std::unexpected(match_error("an unclocked match cannot flag"));
    }
    if (elapsed_milliseconds < *clock) {
        return std::unexpected(match_error("the active clock still has time remaining"));
    }
    *clock = 0;
    const auto opponent = opposite(color);
    undone_manual_result_.reset();
    undone_manual_elapsed_milliseconds_.reset();
    terminal_elapsed_milliseconds_ = elapsed_milliseconds;
    result_ = ChessMatchResult{
        has_possible_mating_material(*position, opponent) ? win_for(opponent)
                                                          : MatchOutcome::draw,
        MatchResultReason::timeout, color, std::nullopt};
    return MatchUpdate{std::nullopt, result_};
}

std::expected<void, Diagnostic> ChessMatch::resign(Color color) {
    if (result_.terminal()) {
        return std::unexpected(match_error("the chess match has already ended"));
    }
    undone_manual_result_.reset();
    undone_manual_elapsed_milliseconds_.reset();
    terminal_elapsed_milliseconds_.reset();
    result_ = {win_for(opposite(color)), MatchResultReason::resignation, color, std::nullopt};
    return {};
}

std::expected<void, Diagnostic> ChessMatch::agree_draw() {
    if (result_.terminal()) {
        return std::unexpected(match_error("the chess match has already ended"));
    }
    undone_manual_result_.reset();
    undone_manual_elapsed_milliseconds_.reset();
    terminal_elapsed_milliseconds_.reset();
    draw_offer_.reset();
    result_ = {MatchOutcome::draw, MatchResultReason::agreed_draw, std::nullopt,
               std::nullopt};
    return {};
}

std::expected<void, Diagnostic> ChessMatch::offer_draw(Color color) {
    if (result_.terminal()) {
        return std::unexpected(match_error("the chess match has already ended"));
    }
    if (draw_offer_ == color) {
        return std::unexpected(match_error("that player already offered a draw"));
    }
    if (draw_offer_ && *draw_offer_ != color) {
        return agree_draw();
    }
    undone_manual_result_.reset();
    undone_manual_elapsed_milliseconds_.reset();
    draw_offer_ = color;
    return {};
}

std::expected<void, Diagnostic> ChessMatch::decline_draw(Color color) {
    if (!draw_offer_ || *draw_offer_ == color) {
        return std::unexpected(match_error("there is no opposing draw offer to decline"));
    }
    undone_manual_result_.reset();
    undone_manual_elapsed_milliseconds_.reset();
    draw_offer_.reset();
    return {};
}

std::size_t ChessMatch::repetition_count(const Position& target,
                                         bool include_candidate) const {
    const auto target_key = repetition_key(target);
    auto cursor = settings_.initial_position;
    std::size_t count = repetition_key(cursor) == target_key ? 1U : 0U;
    for (const auto& ply : std::span<const TimedPly>{history_}.first(history_cursor_)) {
        const auto applied = cursor.apply(ply.move);
        if (!applied) {
            break;
        }
        if (repetition_key(cursor) == target_key) {
            ++count;
        }
    }
    if (include_candidate) {
        ++count;
    }
    return count;
}

std::vector<DrawClaim> ChessMatch::draw_claims() const {
    std::vector<DrawClaim> result;
    if (result_.terminal()) {
        return result;
    }
    const auto current = game_.position();
    if (!current) {
        return result;
    }
    if (repetition_count(*current, false) >= 3U) {
        result.push_back({MatchResultReason::threefold_repetition, std::nullopt});
    }
    if (current->halfmove_clock() >= 100U) {
        result.push_back({MatchResultReason::fifty_move_rule, std::nullopt});
    }
    for (const auto move : current->legal_moves()) {
        auto after = *current;
        if (auto applied = after.apply(move); !applied) {
            continue;
        }
        if (repetition_count(after, true) >= 3U) {
            result.push_back({MatchResultReason::threefold_repetition, move});
        }
        if (after.halfmove_clock() >= 100U) {
            result.push_back({MatchResultReason::fifty_move_rule, move});
        }
    }
    return result;
}

std::expected<void, Diagnostic>
ChessMatch::claim_draw(MatchResultReason reason, std::optional<ChessMove> intended_move) {
    if (reason != MatchResultReason::threefold_repetition &&
        reason != MatchResultReason::fifty_move_rule) {
        return std::unexpected(match_error("the requested result is not a claimable draw"));
    }
    const auto claims = draw_claims();
    if (std::ranges::none_of(claims, [&](const DrawClaim& claim) {
            return claim.reason == reason && claim.intended_move == intended_move;
        })) {
        return std::unexpected(match_error("the requested draw claim is not currently valid"));
    }
    const auto current = game_.position();
    undone_manual_result_.reset();
    undone_manual_elapsed_milliseconds_.reset();
    terminal_elapsed_milliseconds_.reset();
    result_ = {MatchOutcome::draw, reason,
               current ? std::optional<Color>{current->side_to_move()} : std::nullopt,
               intended_move};
    return {};
}

void ChessMatch::refresh_automatic_result() {
    result_ = {};
    terminal_elapsed_milliseconds_.reset();
    const auto position = game_.position();
    const auto status = game_.status();
    if (!position || !status) {
        return;
    }
    if (*status == PositionStatus::checkmate) {
        result_ = {win_for(opposite(position->side_to_move())),
                   MatchResultReason::checkmate, std::nullopt, std::nullopt};
    } else if (*status == PositionStatus::stalemate) {
        result_ = {MatchOutcome::draw, MatchResultReason::stalemate,
                   std::nullopt, std::nullopt};
    } else if (insufficient_material(*position)) {
        result_ = {MatchOutcome::draw, MatchResultReason::insufficient_material,
                   std::nullopt, std::nullopt};
    } else if (repetition_count(*position, false) >= 5U) {
        result_ = {MatchOutcome::draw, MatchResultReason::fivefold_repetition,
                   std::nullopt, std::nullopt};
    } else if (position->halfmove_clock() >= 150U) {
        result_ = {MatchOutcome::draw, MatchResultReason::seventy_five_move_rule,
                   std::nullopt, std::nullopt};
    }
}

void ChessMatch::restore_clock_at_cursor() {
    if (!settings_.time_control.base_milliseconds) {
        white_remaining_.reset();
        black_remaining_.reset();
    } else if (history_cursor_ == 0U) {
        white_remaining_ = *settings_.time_control.base_milliseconds;
        black_remaining_ = *settings_.time_control.base_milliseconds;
    } else {
        white_remaining_ = history_[history_cursor_ - 1U].white_remaining_milliseconds;
        black_remaining_ = history_[history_cursor_ - 1U].black_remaining_milliseconds;
    }
}

std::expected<void, Diagnostic> ChessMatch::undo() {
    if (result_.terminal() && is_manual_result(result_.reason)) {
        undone_manual_result_ = result_;
        undone_manual_elapsed_milliseconds_ = terminal_elapsed_milliseconds_;
        terminal_elapsed_milliseconds_.reset();
        result_ = {};
        restore_clock_at_cursor();
        refresh_automatic_result();
        return {};
    }
    if (history_cursor_ == 0U) {
        return std::unexpected(match_error("there is no chess match command to undo"));
    }
    if (auto undone = game_.undo(); !undone) {
        return std::unexpected(undone.error());
    }
    --history_cursor_;
    restore_clock_at_cursor();
    refresh_automatic_result();
    return {};
}

std::expected<void, Diagnostic> ChessMatch::redo() {
    if (undone_manual_result_) {
        result_ = *undone_manual_result_;
        terminal_elapsed_milliseconds_ = undone_manual_elapsed_milliseconds_;
        if (result_.reason == MatchResultReason::timeout && result_.actor) {
            auto& clock = *result_.actor == Color::white ? white_remaining_ : black_remaining_;
            if (clock) {
                *clock = 0;
            }
        }
        undone_manual_result_.reset();
        undone_manual_elapsed_milliseconds_.reset();
        return {};
    }
    if (result_.terminal()) {
        return std::unexpected(match_error("a terminal chess match cannot redo"));
    }
    if (history_cursor_ >= history_.size()) {
        return std::unexpected(match_error("there is no chess match command to redo"));
    }
    if (auto redone = game_.redo(); !redone) {
        return std::unexpected(redone.error());
    }
    ++history_cursor_;
    restore_clock_at_cursor();
    refresh_automatic_result();
    return {};
}

std::vector<std::byte> ChessMatch::save() const {
    BinaryWriter writer;
    writer.string(archive_magic);
    writer.u32(archive_version);
    writer.string(settings_.white_name);
    writer.string(settings_.black_name);
    writer.string(settings_.initial_position.to_fen());
    write_optional_time(writer, settings_.time_control.base_milliseconds);
    writer.i64(settings_.time_control.increment_milliseconds);
    writer.u64(static_cast<std::uint64_t>(history_cursor_));
    writer.u64(static_cast<std::uint64_t>(history_.size()));
    for (const auto& ply : history_) {
        write_move(writer, ply.move);
        writer.i64(ply.elapsed_milliseconds);
        write_optional_time(writer, ply.white_remaining_milliseconds);
        write_optional_time(writer, ply.black_remaining_milliseconds);
    }
    writer.u8(static_cast<std::uint8_t>(result_.outcome));
    writer.u8(static_cast<std::uint8_t>(result_.reason));
    writer.u8(result_.actor ? static_cast<std::uint8_t>(*result_.actor) : 0xffU);
    writer.boolean(result_.intended_move.has_value());
    if (result_.intended_move) {
        write_move(writer, *result_.intended_move);
    }
    write_optional_time(writer, terminal_elapsed_milliseconds_);
    writer.u8(draw_offer_ ? static_cast<std::uint8_t>(*draw_offer_) : 0xffU);
    writer.boolean(undone_manual_result_.has_value());
    if (undone_manual_result_) {
        writer.u8(static_cast<std::uint8_t>(undone_manual_result_->outcome));
        writer.u8(static_cast<std::uint8_t>(undone_manual_result_->reason));
        writer.u8(undone_manual_result_->actor
                      ? static_cast<std::uint8_t>(*undone_manual_result_->actor)
                      : 0xffU);
        writer.boolean(undone_manual_result_->intended_move.has_value());
        if (undone_manual_result_->intended_move) {
            write_move(writer, *undone_manual_result_->intended_move);
        }
        write_optional_time(writer, undone_manual_elapsed_milliseconds_);
    }
    return std::move(writer).take();
}

std::expected<ChessMatch, Diagnostic>
ChessMatch::load(PythonRuntime& runtime, std::span<const std::byte> archive) {
    if (archive.size() > (1U << 24U)) {
        return std::unexpected(archive_error("chess match archive exceeds 16 MiB"));
    }
    BinaryReader reader{archive};
    if (reader.string() != archive_magic) {
        return std::unexpected(archive_error("unsupported chess match archive header"));
    }
    const auto version = reader.u32();
    if (version == 0U || version > archive_version) {
        return std::unexpected(archive_error("unsupported chess match archive header"));
    }
    ChessMatchSettings settings;
    settings.white_name = reader.string();
    settings.black_name = reader.string();
    const auto initial = Position::from_fen(reader.string());
    settings.time_control.base_milliseconds = read_optional_time(reader);
    settings.time_control.increment_milliseconds = reader.i64();
    const auto cursor = reader.u64();
    const auto count = reader.u64();
    if (!initial || count > maximum_plies || cursor > count ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(archive_error("chess match archive bounds are invalid"));
    }
    settings.initial_position = *initial;
    std::vector<TimedPly> plies;
    plies.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        plies.push_back(TimedPly{read_move(reader), reader.i64(),
                                 read_optional_time(reader), read_optional_time(reader)});
    }
    ChessMatchResult saved_result;
    saved_result.outcome = static_cast<MatchOutcome>(reader.u8());
    saved_result.reason = static_cast<MatchResultReason>(reader.u8());
    const auto actor = reader.u8();
    if (actor <= static_cast<std::uint8_t>(Color::black)) {
        saved_result.actor = static_cast<Color>(actor);
    } else if (actor != 0xffU) {
        reader.invalidate("invalid archived match actor");
    }
    if (reader.boolean()) {
        saved_result.intended_move = read_move(reader);
    }
    const auto saved_terminal_elapsed =
        version >= 2U ? read_optional_time(reader) : std::optional<std::int64_t>{};
    const auto draw_offer = reader.u8();
    std::optional<Color> saved_draw_offer;
    if (draw_offer <= static_cast<std::uint8_t>(Color::black)) {
        saved_draw_offer = static_cast<Color>(draw_offer);
    } else if (draw_offer != 0xffU) {
        reader.invalidate("invalid archived draw offer");
    }
    std::optional<ChessMatchResult> saved_undone_result;
    std::optional<std::int64_t> saved_undone_elapsed;
    if (version >= 2U && reader.boolean()) {
        ChessMatchResult undone;
        undone.outcome = static_cast<MatchOutcome>(reader.u8());
        undone.reason = static_cast<MatchResultReason>(reader.u8());
        const auto undone_actor = reader.u8();
        if (undone_actor <= static_cast<std::uint8_t>(Color::black)) {
            undone.actor = static_cast<Color>(undone_actor);
        } else if (undone_actor != 0xffU) {
            reader.invalidate("invalid archived undone-result actor");
        }
        if (reader.boolean()) {
            undone.intended_move = read_move(reader);
        }
        saved_undone_elapsed = read_optional_time(reader);
        if (!undone.terminal() || !is_manual_result(undone.reason) ||
            undone.outcome > MatchOutcome::draw ||
            undone.reason > MatchResultReason::insufficient_material ||
            (undone.reason == MatchResultReason::timeout) != saved_undone_elapsed.has_value()) {
            reader.invalidate("invalid archived undone match command");
        }
        saved_undone_result = undone;
    }
    if ((version >= 2U && (saved_result.reason == MatchResultReason::timeout) !=
                              saved_terminal_elapsed.has_value()) ||
        (saved_terminal_elapsed &&
         (*saved_terminal_elapsed < 0 ||
          *saved_terminal_elapsed > maximum_base_milliseconds)) ||
        (saved_undone_elapsed &&
         (*saved_undone_elapsed < 0 ||
          *saved_undone_elapsed > maximum_base_milliseconds)) ||
        !reader.ok() || !reader.at_end() ||
        saved_result.outcome > MatchOutcome::draw ||
        saved_result.reason > MatchResultReason::insufficient_material) {
        return std::unexpected(archive_error("chess match archive is malformed"));
    }

    auto created = create(runtime, std::move(settings));
    if (!created) {
        return std::unexpected(created.error());
    }
    auto result = std::move(*created);
    for (const auto& ply : plies) {
        auto committed = result.submit(ply.move, ply.elapsed_milliseconds);
        if (!committed || !committed->events ||
            result.white_remaining_ != ply.white_remaining_milliseconds ||
            result.black_remaining_ != ply.black_remaining_milliseconds) {
            return std::unexpected(archive_error("chess match archive replay diverged"));
        }
    }
    while (result.history_cursor_ > cursor) {
        if (auto undone = result.undo(); !undone) {
            return std::unexpected(archive_error("chess match cursor could not be restored"));
        }
    }
    if (cursor == count && saved_result.terminal() && !result.result_.terminal()) {
        switch (saved_result.reason) {
        case MatchResultReason::resignation:
            if (!saved_result.actor || !result.resign(*saved_result.actor)) {
                return std::unexpected(archive_error("archived resignation is invalid"));
            }
            break;
        case MatchResultReason::agreed_draw:
            if (!result.agree_draw()) {
                return std::unexpected(archive_error("archived agreed draw is invalid"));
            }
            break;
        case MatchResultReason::timeout:
            if (version >= 2U) {
                if (!saved_result.actor || !saved_terminal_elapsed ||
                    !result.flag(*saved_result.actor, *saved_terminal_elapsed)) {
                    return std::unexpected(archive_error("archived timeout is invalid"));
                }
            } else {
                result.result_ = saved_result;
                if (saved_result.actor) {
                    auto& clock = *saved_result.actor == Color::white
                                      ? result.white_remaining_ : result.black_remaining_;
                    if (clock) {
                        *clock = 0;
                    }
                }
            }
            break;
        case MatchResultReason::threefold_repetition:
        case MatchResultReason::fifty_move_rule:
            if (!result.claim_draw(saved_result.reason, saved_result.intended_move)) {
                return std::unexpected(archive_error("archived draw claim is invalid"));
            }
            break;
        default:
            break;
        }
    }
    if (result.result_ != saved_result) {
        return std::unexpected(archive_error("archived chess match result diverged"));
    }
    if (saved_undone_result) {
        const auto position = result.game_.position();
        bool valid_undone = position.has_value();
        switch (saved_undone_result->reason) {
        case MatchResultReason::timeout: {
            const auto clock = saved_undone_result->actor
                                   ? result.remaining(*saved_undone_result->actor)
                                   : std::nullopt;
            const auto opponent = saved_undone_result->actor
                                      ? opposite(*saved_undone_result->actor)
                                      : Color::white;
            const auto expected = position && saved_undone_result->actor
                                      ? (has_possible_mating_material(*position, opponent)
                                             ? win_for(opponent) : MatchOutcome::draw)
                                      : MatchOutcome::ongoing;
            valid_undone = valid_undone && saved_undone_result->actor &&
                           saved_undone_elapsed && clock &&
                           !saved_undone_result->intended_move &&
                           position->side_to_move() == *saved_undone_result->actor &&
                           *saved_undone_elapsed >= *clock &&
                           saved_undone_result->outcome == expected;
            break;
        }
        case MatchResultReason::resignation:
            valid_undone = valid_undone && saved_undone_result->actor &&
                           !saved_undone_result->intended_move &&
                           saved_undone_result->outcome ==
                               win_for(opposite(*saved_undone_result->actor));
            break;
        case MatchResultReason::agreed_draw:
            valid_undone = valid_undone && !saved_undone_result->actor &&
                           !saved_undone_result->intended_move &&
                           saved_undone_result->outcome == MatchOutcome::draw;
            break;
        case MatchResultReason::threefold_repetition:
        case MatchResultReason::fifty_move_rule: {
            const auto claims = result.draw_claims();
            valid_undone = valid_undone &&
                           saved_undone_result->outcome == MatchOutcome::draw &&
                           saved_undone_result->actor &&
                           *saved_undone_result->actor == position->side_to_move() &&
                           std::ranges::any_of(claims, [&](const DrawClaim& claim) {
                               return claim.reason == saved_undone_result->reason &&
                                      claim.intended_move ==
                                          saved_undone_result->intended_move;
                           });
            break;
        }
        default:
            valid_undone = false;
            break;
        }
        if (!valid_undone) {
            return std::unexpected(archive_error("archived undone match command is invalid"));
        }
    }
    result.draw_offer_ = saved_draw_offer;
    result.terminal_elapsed_milliseconds_ = saved_terminal_elapsed;
    result.undone_manual_result_ = saved_undone_result;
    result.undone_manual_elapsed_milliseconds_ = saved_undone_elapsed;
    return result;
}

std::uint64_t ChessMatch::match_hash() const { return canonical_hash(save()); }

std::string match_result_token(const ChessMatchResult& result) {
    switch (result.outcome) {
    case MatchOutcome::white_wins:
        return "1-0";
    case MatchOutcome::black_wins:
        return "0-1";
    case MatchOutcome::draw:
        return "1/2-1/2";
    case MatchOutcome::ongoing:
        return "*";
    }
    return "*";
}

std::string match_result_description(const ChessMatchResult& result) {
    switch (result.reason) {
    case MatchResultReason::checkmate:
        return "checkmate";
    case MatchResultReason::stalemate:
        return "stalemate";
    case MatchResultReason::resignation:
        return "resignation";
    case MatchResultReason::timeout:
        return "time forfeit";
    case MatchResultReason::agreed_draw:
        return "draw by agreement";
    case MatchResultReason::threefold_repetition:
        return "draw by threefold repetition";
    case MatchResultReason::fifty_move_rule:
        return "draw by fifty-move rule";
    case MatchResultReason::fivefold_repetition:
        return "draw by fivefold repetition";
    case MatchResultReason::seventy_five_move_rule:
        return "draw by seventy-five-move rule";
    case MatchResultReason::insufficient_material:
        return "draw by insufficient material";
    case MatchResultReason::none:
        return result.terminal() ? "game over" : "game in progress";
    }
    return "game in progress";
}

} // namespace ludus::chess
