#include "ludus/chess/presentation.hpp"
#include "ludus/chess/pgn.hpp"

#include "ludus/render/animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ludus::chess {
namespace {

constexpr PlayerId white_player{0U, 1U};
constexpr PlayerId black_player{1U, 1U};
constexpr std::uint32_t sprite_size = 64U;

Diagnostic presentation_error(std::string message) {
    return Diagnostic{DiagnosticCode::invalid_state, std::move(message), {}};
}

std::expected<TagId, Diagnostic> find_tag(const GameState& state, std::string_view name) {
    const auto id = state.symbols().tags.find(name);
    if (!id) {
        return std::unexpected(presentation_error(id.error()));
    }
    return *id;
}

std::expected<PropertyId, Diagnostic> find_property(const GameState& state,
                                                    std::string_view name) {
    const auto id = state.symbols().properties.find(name);
    if (!id) {
        return std::unexpected(presentation_error(id.error()));
    }
    return *id;
}

constexpr Vec2 square_center(std::uint32_t square) noexcept {
    return {static_cast<float>(square % 8U) - 3.5F,
            static_cast<float>(square / 8U) - 3.5F};
}

constexpr Rect square_bounds(std::uint32_t square) noexcept {
    const auto center = square_center(square);
    return {{center.x - 0.5F, center.y - 0.5F},
            {center.x + 0.5F, center.y + 0.5F}};
}

void set_alpha(ImageRgba& image, int x, int y, std::uint8_t alpha = 255U) {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
        y >= static_cast<int>(image.height)) {
        return;
    }
    const auto offset =
        (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4U;
    image.pixels[offset] = 255U;
    image.pixels[offset + 1U] = 255U;
    image.pixels[offset + 2U] = 255U;
    image.pixels[offset + 3U] = alpha;
}

void fill_rectangle(ImageRgba& image, int left, int top, int right, int bottom) {
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            set_alpha(image, x, y);
        }
    }
}

void fill_circle(ImageRgba& image, int center_x, int center_y, int radius) {
    const int radius_squared = radius * radius;
    for (int y = center_y - radius; y <= center_y + radius; ++y) {
        for (int x = center_x - radius; x <= center_x + radius; ++x) {
            const int delta_x = x - center_x;
            const int delta_y = y - center_y;
            if (delta_x * delta_x + delta_y * delta_y <= radius_squared) {
                set_alpha(image, x, y);
            }
        }
    }
}

void fill_triangle(ImageRgba& image, Vec2 first, Vec2 second, Vec2 third) {
    const auto edge = [](Vec2 a, Vec2 b, Vec2 point) {
        return (point.x - a.x) * (b.y - a.y) - (point.y - a.y) * (b.x - a.x);
    };
    const int min_x = static_cast<int>(std::floor(std::min({first.x, second.x, third.x})));
    const int max_x = static_cast<int>(std::ceil(std::max({first.x, second.x, third.x})));
    const int min_y = static_cast<int>(std::floor(std::min({first.y, second.y, third.y})));
    const int max_y = static_cast<int>(std::ceil(std::max({first.y, second.y, third.y})));
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const Vec2 point{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
            const auto first_edge = edge(first, second, point);
            const auto second_edge = edge(second, third, point);
            const auto third_edge = edge(third, first, point);
            if ((first_edge >= 0.0F && second_edge >= 0.0F && third_edge >= 0.0F) ||
                (first_edge <= 0.0F && second_edge <= 0.0F && third_edge <= 0.0F)) {
                set_alpha(image, x, y);
            }
        }
    }
}

ImageRgba make_piece_sprite(PieceType type) {
    ImageRgba image{sprite_size, sprite_size,
                    std::vector<std::uint8_t>(sprite_size * sprite_size * 4U, 0U)};
    fill_rectangle(image, 15, 51, 49, 56);
    fill_rectangle(image, 19, 47, 45, 52);

    switch (type) {
    case PieceType::pawn:
        fill_circle(image, 32, 21, 10);
        fill_triangle(image, {25.0F, 28.0F}, {39.0F, 28.0F}, {43.0F, 48.0F});
        fill_triangle(image, {25.0F, 28.0F}, {43.0F, 48.0F}, {21.0F, 48.0F});
        break;
    case PieceType::knight:
        fill_triangle(image, {19.0F, 47.0F}, {25.0F, 25.0F}, {43.0F, 47.0F});
        fill_triangle(image, {24.0F, 28.0F}, {38.0F, 12.0F}, {47.0F, 28.0F});
        fill_circle(image, 39, 22, 2);
        break;
    case PieceType::bishop:
        fill_circle(image, 32, 20, 10);
        fill_triangle(image, {25.0F, 29.0F}, {39.0F, 29.0F}, {43.0F, 48.0F});
        fill_triangle(image, {25.0F, 29.0F}, {43.0F, 48.0F}, {21.0F, 48.0F});
        for (int offset = 0; offset < 3; ++offset) {
            for (int point = 0; point < 15; ++point) {
                set_alpha(image, 27 + point, 14 + point + offset, 0U);
            }
        }
        break;
    case PieceType::rook:
        fill_rectangle(image, 20, 20, 44, 48);
        fill_rectangle(image, 17, 15, 24, 24);
        fill_rectangle(image, 28, 15, 36, 24);
        fill_rectangle(image, 40, 15, 47, 24);
        break;
    case PieceType::queen:
        fill_triangle(image, {18.0F, 19.0F}, {46.0F, 19.0F}, {42.0F, 48.0F});
        fill_triangle(image, {18.0F, 19.0F}, {42.0F, 48.0F}, {22.0F, 48.0F});
        fill_circle(image, 19, 15, 5);
        fill_circle(image, 32, 11, 5);
        fill_circle(image, 45, 15, 5);
        break;
    case PieceType::king:
        fill_circle(image, 32, 27, 11);
        fill_triangle(image, {24.0F, 33.0F}, {40.0F, 33.0F}, {43.0F, 48.0F});
        fill_triangle(image, {24.0F, 33.0F}, {43.0F, 48.0F}, {21.0F, 48.0F});
        fill_rectangle(image, 29, 7, 35, 20);
        fill_rectangle(image, 24, 11, 40, 16);
        break;
    case PieceType::none:
        break;
    }
    return image;
}

std::string status_text(const Position& position, PositionStatus status,
                        std::size_t legal_count) {
    if (status == PositionStatus::checkmate) {
        return position.side_to_move() == Color::white ? "Checkmate — Black wins"
                                                       : "Checkmate — White wins";
    }
    if (status == PositionStatus::stalemate) {
        return "Stalemate";
    }
    std::string result =
        position.side_to_move() == Color::white ? "White to move" : "Black to move";
    if (position.in_check(position.side_to_move())) {
        result += " — check";
    }
    result += " — " + std::to_string(legal_count) + " legal moves";
    return result;
}

std::array<std::array<int, 7U>, 2U> piece_counts(const Position& position) {
    std::array<std::array<int, 7U>, 2U> result{};
    for (std::uint8_t square = 0U; square < 64U; ++square) {
        const auto piece = position.piece_at(square);
        if (!piece.empty()) {
            ++result[piece.color == Color::white ? 0U : 1U]
                    [static_cast<std::size_t>(piece.type)];
        }
    }
    return result;
}

int material_score(const std::array<int, 7U>& pieces) {
    constexpr std::array<int, 7U> values{0, 1, 3, 3, 5, 9, 0};
    int result = 0;
    for (std::size_t type = 1U; type < values.size(); ++type) {
        result += pieces[type] * values[type];
    }
    return result;
}

std::string participant_title(std::string_view name, int balance) {
    if (balance <= 0) {
        return std::string{name};
    }
    return std::string{name} + "  +" + std::to_string(balance);
}

} // namespace

std::expected<ChessPresentation, Diagnostic> ChessPresentation::create(const ChessGame& game) {
    const auto piece_tag = find_tag(game.session().state(), "chess_piece");
    const auto piece_type = find_property(game.session().state(), "piece_type");
    if (!piece_tag) {
        return std::unexpected(piece_tag.error());
    }
    if (!piece_type) {
        return std::unexpected(piece_type.error());
    }
    return ChessPresentation{*piece_tag, *piece_type};
}

std::expected<RenderSnapshot, Diagnostic>
ChessPresentation::build(const ChessGame& game, std::uint64_t revision,
                         const RenderSnapshot* previous, const EventBatch* batch,
                         std::chrono::steady_clock::time_point animation_start) const {
    RenderSnapshot result;
    result.revision = revision;
    result.static_revision = 0x4348455353424f41ULL;
    result.dynamic_revision = revision;
    result.world_bounds = {{-4.55F, -4.55F}, {4.55F, 4.55F}};
    result.spaces.reserve(64U);
    for (std::uint32_t square = 0U; square < 64U; ++square) {
        const auto rank = square / 8U;
        const auto file = square % 8U;
        const bool light = (rank + file) % 2U != 0U;
        const auto color = light ? ludus::Color{0.48F, 0.39F, 0.27F, 1.0F}
                                 : ludus::Color{0.105F, 0.13F, 0.17F, 1.0F};
        result.spaces.push_back(SpaceVisual{SpaceId{square, 1U}, square_bounds(square), color,
                                            SpaceShape::rectangle,
                                            {0.035F, 0.025F, 0.02F, 0.4F}, 0.012F});
    }
    result.shapes.push_back(ShapeVisual{{{-4.12F, -4.12F}, {4.12F, 4.12F}},
                                        {0.0F, 0.0F, 0.0F, 0.0F},
                                        SpaceShape::rounded_rectangle,
                                        {0.68F, 0.49F, 0.20F, 0.92F}, 0.045F});

    const auto moves = game.legal_moves();
    const auto position = game.position();
    const auto status = game.status();
    if (!moves) {
        return std::unexpected(moves.error());
    }
    if (!position) {
        return std::unexpected(position.error());
    }
    if (!status) {
        return std::unexpected(status.error());
    }
    const auto history = game.move_history();
    if (!history.empty()) {
        const auto& last = history.back();
        for (const auto square : {last.from, last.to}) {
            const auto bounds = square_bounds(square);
            constexpr float corner = 0.19F;
            constexpr float width = 0.045F;
            const auto color = ludus::Color{0.96F, 0.72F, 0.24F, 0.78F};
            for (const auto x : {bounds.minimum.x, bounds.maximum.x - corner}) {
                for (const auto y : {bounds.minimum.y, bounds.maximum.y - width}) {
                    result.shapes.push_back(ShapeVisual{{{x, y}, {x + corner, y + width}},
                                                        color, SpaceShape::rectangle, {},
                                                        0.0F, 0.0F, 2.0F});
                }
            }
            for (const auto x : {bounds.minimum.x, bounds.maximum.x - width}) {
                for (const auto y : {bounds.minimum.y, bounds.maximum.y - corner}) {
                    result.shapes.push_back(ShapeVisual{{{x, y}, {x + width, y + corner}},
                                                        color, SpaceShape::rectangle, {},
                                                        0.0F, 0.0F, 2.0F});
                }
            }
        }
    }

    const auto coordinate_color = ludus::Color{0.74F, 0.63F, 0.42F, 0.9F};
    for (std::uint32_t file = 0U; file < 8U; ++file) {
        result.texts.push_back(TextVisual{std::string(1U, static_cast<char>('a' + file)),
                                          {static_cast<float>(file) - 3.5F, -4.28F}, 10.0F,
                                          coordinate_color});
    }
    for (std::uint32_t rank = 0U; rank < 8U; ++rank) {
        result.texts.push_back(TextVisual{std::to_string(rank + 1U),
                                          {-4.29F, static_cast<float>(rank) - 3.5F}, 10.0F,
                                          coordinate_color});
    }

    std::array<std::optional<EntityId>, 64U> entities_by_square{};
    std::optional<Vec2> checked_king;
    for (const auto entity_id : game.session().state().entities().entities()) {
        const auto entity = game.session().state().entities().snapshot(entity_id);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        if (!entity->tags.contains(piece_tag_)) {
            continue;
        }
        if (!entity->location || entity->location->index() >= 64U ||
            (entity->owner != white_player && entity->owner != black_player)) {
            return std::unexpected(presentation_error("chess piece presentation is malformed"));
        }
        const auto* value = entity->properties.find(piece_type_);
        const auto* integer = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
        if (integer == nullptr || *integer < static_cast<std::int64_t>(PieceType::pawn) ||
            *integer > static_cast<std::int64_t>(PieceType::king)) {
            return std::unexpected(presentation_error("chess piece presentation type is invalid"));
        }
        const auto type = static_cast<PieceType>(*integer);
        const auto square = entity->location->index();
        entities_by_square[square] = entity_id;
        const bool white = entity->owner == white_player;
        const bool in_check = type == PieceType::king &&
                              ((white && position->side_to_move() == Color::white) ||
                               (!white && position->side_to_move() == Color::black)) &&
                              position->in_check(position->side_to_move());
        PieceVisual visual{entity_id,
                           *entity->location,
                           square_center(square),
                           {0.80F, 1.02F},
                           SpriteId{static_cast<std::uint32_t>(type) - 1U +
                                    (white ? 0U : 6U)},
                           white ? ludus::Color{1.0F, 1.0F, 1.0F, 1.0F}
                                 : ludus::Color{1.12F, 1.18F, 1.28F, 1.0F},
                           1.0F};
        if (in_check) {
            visual.outline = {0.98F, 0.14F, 0.12F, 0.95F};
            visual.outline_width = 0.08F;
            checked_king = visual.center;
        }
        result.pieces.push_back(std::move(visual));
    }
    std::ranges::sort(result.pieces, {}, &PieceVisual::id);
    result.actions.reserve(moves->size());
    for (const auto move : *moves) {
        const auto actor = entities_by_square[move.from];
        if (!actor) {
            return std::unexpected(presentation_error("legal chess move actor is missing"));
        }
        const bool capture = has_flag(move.flags, MoveFlag::capture) ||
                             has_flag(move.flags, MoveFlag::en_passant);
        const bool castle = has_flag(move.flags, MoveFlag::king_castle) ||
                            has_flag(move.flags, MoveFlag::queen_castle);
        const auto label = move.promotion != PieceType::none
                               ? std::string{"Promote"}
                           : castle ? std::string{"Castle"}
                           : capture ? std::string{"Capture"}
                                     : std::string{"Move"};
        const auto kind = move.promotion != PieceType::none
                              ? ActionVisualKind::promotion
                          : castle ? ActionVisualKind::castle
                          : capture ? ActionVisualKind::capture
                                    : ActionVisualKind::move;
        result.actions.push_back(ActionHint{encode_action_token(move), *actor,
                                            SpaceId{move.from, 1U}, SpaceId{move.to, 1U},
                                            static_cast<std::uint32_t>(move.promotion),
                                            kind,
                                            label, std::nullopt,
                                            ActionTargetSemantics::space});
    }
    result.status = status_text(*position, *status, moves->size());
    if (batch != nullptr && !history.empty()) {
        const auto& last = history.back();
        if (has_flag(last.flags, MoveFlag::capture) ||
            has_flag(last.flags, MoveFlag::en_passant)) {
            result.effects.push_back(EffectVisual{EffectKind::capture_fade,
                                                  square_center(last.to),
                                                  square_center(last.to),
                                                  {0.96F, 0.32F, 0.12F, 0.7F}, 0.42F,
                                                  animation_start,
                                                  std::chrono::milliseconds{360}, 15.0F,
                                                  EffectBlend::additive, 0.65F, 1.45F});
        }
        if (last.promotion != PieceType::none) {
            result.effects.push_back(EffectVisual{EffectKind::promotion,
                                                  square_center(last.to),
                                                  square_center(last.to),
                                                  {0.72F, 0.46F, 1.0F, 0.72F}, 0.5F,
                                                  animation_start,
                                                  std::chrono::milliseconds{650}, 15.0F,
                                                  EffectBlend::additive, 0.4F, 1.35F});
        }
    }
    if (batch != nullptr && checked_king) {
        result.effects.push_back(EffectVisual{EffectKind::check, *checked_king, *checked_king,
                                              {0.96F, 0.08F, 0.12F, 0.52F}, 0.58F,
                                              animation_start,
                                              std::chrono::milliseconds{700}, 15.0F,
                                              EffectBlend::additive, 0.9F, 1.12F});
    }
    if (previous != nullptr && batch != nullptr) {
        result.animations = EventAnimationAdapter{}.adapt(*batch, *previous, result,
                                                          animation_start);
    }
    return result;
}

std::expected<PlayerView, Diagnostic>
ChessPresentation::build_view(const ChessMatch& match, std::uint64_t revision,
                              const RenderSnapshot* previous, const EventBatch* batch,
                              std::chrono::steady_clock::time_point animation_start,
                              bool reduced_motion) const {
    auto rendered = build(match.game(), revision, previous, batch, animation_start);
    if (!rendered) {
        return std::unexpected(rendered.error());
    }
    PlayerView result;
    result.render = std::move(*rendered);
    if (reduced_motion) {
        result.render.animations.clear();
        result.render.effects.clear();
    }
    const auto current = match.game().position();
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto initial_counts = piece_counts(match.settings().initial_position);
    const auto current_counts = piece_counts(*current);
    const auto white_material = material_score(current_counts[0]);
    const auto black_material = material_score(current_counts[1]);
    const auto terminal = match.result().terminal();
    result.participants = {
        ParticipantView{ParticipantSide::first, match.settings().white_name,
                        participant_title("Ivory Reliquary",
                                          std::max(white_material - black_material, 0)),
                        std::nullopt,
                        !terminal && current->side_to_move() == Color::white},
        ParticipantView{ParticipantSide::second, match.settings().black_name,
                        participant_title("Blackened Iron",
                                          std::max(black_material - white_material, 0)),
                        std::nullopt,
                        !terminal && current->side_to_move() == Color::black},
    };
    if (match.settings().time_control.clocked()) {
        result.clocks = {
            ClockView{ParticipantSide::first, match.remaining(Color::white).value_or(0),
                      match.settings().time_control.increment_milliseconds,
                      !terminal && current->side_to_move() == Color::white, terminal,
                      match.remaining(Color::white) == 0},
            ClockView{ParticipantSide::second, match.remaining(Color::black).value_or(0),
                      match.settings().time_control.increment_milliseconds,
                      !terminal && current->side_to_move() == Color::black, terminal,
                      match.remaining(Color::black) == 0},
        };
    }
    constexpr std::array<std::string_view, 7U> names{
        "", "Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};
    constexpr std::array<int, 7U> values{0, 1, 3, 3, 5, 9, 0};
    for (std::size_t color = 0U; color < 2U; ++color) {
        for (std::size_t type = 1U; type < 7U; ++type) {
            const auto captured = std::max(initial_counts[color][type] -
                                               current_counts[color][type],
                                           0);
            if (captured == 0) {
                continue;
            }
            result.captured_items.push_back(CapturedItemView{
                color == 0U ? ParticipantSide::first : ParticipantSide::second,
                std::string{names[type]}, captured, captured * values[type],
                SpriteId{static_cast<std::uint32_t>(type - 1U + (color == 0U ? 0U : 6U))}});
        }
    }
    auto timeline_position = match.settings().initial_position;
    const auto history = match.history();
    result.timeline.reserve(history.size());
    for (std::size_t index = 0U; index < history.size(); ++index) {
        const auto san = to_san(timeline_position, history[index].move);
        result.timeline.push_back(TimelineEntryView{
            index, timeline_position.fullmove_number(),
            timeline_position.side_to_move() == Color::white
                ? ParticipantSide::first : ParticipantSide::second,
            san ? *san : to_uci(history[index].move), index + 1U == history.size(), false});
        if (const auto applied = timeline_position.apply(history[index].move); !applied) {
            return std::unexpected(applied.error());
        }
    }
    result.match_controls = MatchControlView{
        match.history_cursor() != 0U ||
            (terminal && (match.result().reason == MatchResultReason::resignation ||
                          match.result().reason == MatchResultReason::timeout ||
                          match.result().reason == MatchResultReason::agreed_draw ||
                          match.result().reason == MatchResultReason::threefold_repetition ||
                          match.result().reason == MatchResultReason::fifty_move_rule)),
        !terminal && match.history_cursor() < match.history_size(), !terminal, !terminal,
        false};
    for (const auto& claim : match.draw_claims()) {
        const auto label = claim.reason == MatchResultReason::threefold_repetition
                               ? "Claim draw by threefold repetition"
                               : "Claim draw by fifty-move rule";
        result.draw_claims.push_back(DrawClaimView{
            label, claim.intended_move
                       ? std::optional<std::uint64_t>{encode_action_token(*claim.intended_move)}
                       : std::nullopt});
        if (claim.intended_move) {
            const auto token = encode_action_token(*claim.intended_move);
            const auto action = std::ranges::find(result.render.actions, token,
                                                  &ActionHint::token);
            if (action != result.render.actions.end()) {
                action->kind = ActionVisualKind::draw_claim;
            }
        }
    }
    if (terminal) {
        std::string title;
        if (match.result().outcome == MatchOutcome::white_wins) {
            title = match.settings().white_name + " prevails";
        } else if (match.result().outcome == MatchOutcome::black_wins) {
            title = match.settings().black_name + " prevails";
        } else {
            title = "The match is drawn";
        }
        result.match_result = MatchResultView{
            std::move(title), match_result_description(match.result()),
            match_result_token(match.result()), match.result().outcome == MatchOutcome::draw};
    }
    result.text_exports = {
        TextExportView{"FEN", current->to_fen()},
        TextExportView{"PGN", export_pgn(match)},
    };
    return result;
}

std::expected<TextureAtlas, Diagnostic> make_default_chess_atlas() {
    std::vector<ImageRgba> sprites;
    sprites.reserve(12U);
    for (const auto type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                            PieceType::rook, PieceType::queen, PieceType::king}) {
        sprites.push_back(make_piece_sprite(type));
    }
    for (const auto type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                            PieceType::rook, PieceType::queen, PieceType::king}) {
        auto iron = make_piece_sprite(type);
        for (std::size_t offset = 0U; offset < iron.pixels.size(); offset += 4U) {
            iron.pixels[offset] = 48U;
            iron.pixels[offset + 1U] = 54U;
            iron.pixels[offset + 2U] = 68U;
        }
        sprites.push_back(std::move(iron));
    }
    return TextureAtlas::pack(sprites, 2U);
}

std::expected<TextureAtlas, Diagnostic> load_chess_atlas(const std::filesystem::path& path) {
    auto image = load_ppm_rgba(path);
    if (!image) {
        return std::unexpected(image.error());
    }
    return TextureAtlas::from_grid(std::move(*image), 6U, 1U);
}

} // namespace ludus::chess
