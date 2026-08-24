#include "ludus/chess/pgn.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <sstream>

namespace ludus::chess {
namespace {

constexpr std::size_t maximum_pgn_bytes = 1U << 20U;

struct Cursor {
    std::string_view source;
    std::size_t offset{0U};
    std::size_t line{1U};
    std::size_t column{1U};

    [[nodiscard]] bool end() const noexcept { return offset >= source.size(); }
    [[nodiscard]] char peek() const noexcept { return end() ? '\0' : source[offset]; }
    char take() noexcept {
        const auto value = peek();
        if (!end()) {
            ++offset;
            if (value == '\n') {
                ++line;
                column = 1U;
            } else {
                ++column;
            }
        }
        return value;
    }
    void whitespace() noexcept {
        while (!end() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
            take();
        }
    }
};

PgnDiagnostic error_at(const Cursor& cursor, std::string message) {
    return {std::move(message), cursor.line, cursor.column};
}

std::string normalized_san(std::string_view value) {
    std::string result{value};
    std::ranges::replace(result, '0', 'O');
    while (!result.empty() && (result.back() == '!' || result.back() == '?')) {
        result.pop_back();
    }
    if (result.ends_with("e.p.")) {
        result.resize(result.size() - 4U);
    } else if (result.ends_with("ep")) {
        result.resize(result.size() - 2U);
    }
    return result;
}

std::optional<std::uint32_t> symbolic_nag(std::string_view value) noexcept {
    if (value.ends_with("!!")) {
        return 3U;
    }
    if (value.ends_with("??")) {
        return 4U;
    }
    if (value.ends_with("!?")) {
        return 5U;
    }
    if (value.ends_with("?!")) {
        return 6U;
    }
    if (value.ends_with("!")) {
        return 1U;
    }
    if (value.ends_with("?")) {
        return 2U;
    }
    return std::nullopt;
}

bool valid_result(std::string_view token) noexcept {
    return token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "*";
}

bool is_move_number(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    std::size_t digits = 0U;
    while (digits < token.size() &&
           std::isdigit(static_cast<unsigned char>(token[digits])) != 0) {
        ++digits;
    }
    return digits != 0U && digits < token.size() &&
           std::ranges::all_of(token.substr(digits), [](char value) { return value == '.'; });
}

std::optional<std::int64_t> parse_clock(std::string_view comment) {
    const auto marker = comment.find("[%clk");
    if (marker == std::string_view::npos) {
        return std::nullopt;
    }
    const auto close = comment.find(']', marker);
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    auto value = comment.substr(marker + 5U, close - marker - 5U);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1U);
    }
    std::array<std::string_view, 3U> parts;
    std::size_t count = 0U;
    while (count < parts.size()) {
        const auto separator = value.find(':');
        parts[count++] = value.substr(0U, separator);
        if (separator == std::string_view::npos) {
            break;
        }
        value.remove_prefix(separator + 1U);
    }
    if (count != 3U) {
        return std::nullopt;
    }
    std::int64_t hours = 0;
    std::int64_t minutes = 0;
    const auto integer = [](std::string_view input, std::int64_t& output) {
        const auto [end, code] = std::from_chars(input.data(), input.data() + input.size(), output);
        return code == std::errc{} && end == input.data() + input.size();
    };
    if (!integer(parts[0], hours) || !integer(parts[1], minutes) || hours < 0 ||
        minutes < 0 || minutes >= 60) {
        return std::nullopt;
    }
    const auto decimal = parts[2].find('.');
    std::int64_t seconds = 0;
    if (!integer(parts[2].substr(0U, decimal), seconds) || seconds < 0 || seconds >= 60) {
        return std::nullopt;
    }
    std::int64_t milliseconds = 0;
    if (decimal != std::string_view::npos) {
        auto fraction = parts[2].substr(decimal + 1U);
        if (fraction.empty() || fraction.size() > 3U || !integer(fraction, milliseconds)) {
            return std::nullopt;
        }
        for (std::size_t index = fraction.size(); index < 3U; ++index) {
            milliseconds *= 10;
        }
    }
    if (hours > (std::numeric_limits<std::int64_t>::max() / 3'600'000)) {
        return std::nullopt;
    }
    return hours * 3'600'000 + minutes * 60'000 + seconds * 1'000 + milliseconds;
}

std::optional<TimeControl> parse_time_control(std::string_view value) {
    if (value.empty() || value == "-") {
        return TimeControl{};
    }
    const auto plus = value.find('+');
    const auto base_text = value.substr(0U, plus);
    const auto increment_text = plus == std::string_view::npos
                                    ? std::string_view{"0"}
                                    : value.substr(plus + 1U);
    std::int64_t base_seconds = 0;
    std::int64_t increment_seconds = 0;
    const auto parse = [](std::string_view input, std::int64_t& output) {
        const auto [end, code] = std::from_chars(input.data(), input.data() + input.size(), output);
        return code == std::errc{} && end == input.data() + input.size();
    };
    if (!parse(base_text, base_seconds) || !parse(increment_text, increment_seconds) ||
        base_seconds <= 0 || increment_seconds < 0 ||
        base_seconds > ChessMatch::maximum_base_milliseconds / 1'000 ||
        increment_seconds > ChessMatch::maximum_increment_milliseconds / 1'000) {
        return std::nullopt;
    }
    return TimeControl{base_seconds * 1'000, increment_seconds * 1'000};
}

std::string escaped_tag(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

std::string clock_text(std::int64_t milliseconds) {
    milliseconds = std::max<std::int64_t>(0, milliseconds);
    const auto hours = milliseconds / 3'600'000;
    milliseconds %= 3'600'000;
    const auto minutes = milliseconds / 60'000;
    milliseconds %= 60'000;
    const auto seconds = milliseconds / 1'000;
    const auto remainder = milliseconds % 1'000;
    std::ostringstream output;
    output << hours << ':';
    if (minutes < 10) {
        output << '0';
    }
    output << minutes << ':';
    if (seconds < 10) {
        output << '0';
    }
    output << seconds;
    if (remainder != 0) {
        output << '.';
        if (remainder < 100) {
            output << '0';
        }
        if (remainder < 10) {
            output << '0';
        }
        output << remainder;
    }
    return output.str();
}

std::string termination(const ChessMatchResult& result) {
    switch (result.reason) {
    case MatchResultReason::checkmate: return "checkmate";
    case MatchResultReason::stalemate: return "stalemate";
    case MatchResultReason::resignation: return "resignation";
    case MatchResultReason::timeout: return "time forfeit";
    case MatchResultReason::agreed_draw: return "draw agreement";
    case MatchResultReason::threefold_repetition: return "threefold repetition";
    case MatchResultReason::fifty_move_rule: return "fifty-move rule";
    case MatchResultReason::fivefold_repetition: return "fivefold repetition";
    case MatchResultReason::seventy_five_move_rule: return "seventy-five-move rule";
    case MatchResultReason::insufficient_material: return "insufficient material";
    case MatchResultReason::none: return "unterminated";
    }
    return "unterminated";
}

} // namespace

std::optional<std::string_view> PgnDocument::tag(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(tags, [name](const auto& record) {
        return record.first == name;
    });
    return found == tags.end() ? std::nullopt
                               : std::optional<std::string_view>{found->second};
}

std::expected<ChessMove, PgnDiagnostic>
resolve_san(const Position& position, std::string_view san,
            std::size_t line, std::size_t column) {
    const auto wanted = normalized_san(san);
    std::optional<ChessMove> resolved;
    for (const auto move : position.legal_moves()) {
        const auto generated = to_san(position, move);
        if (generated && normalized_san(*generated) == wanted) {
            if (resolved) {
                return std::unexpected(PgnDiagnostic{"ambiguous SAN move '" +
                                                         std::string{san} + "'",
                                                     line, column});
            }
            resolved = move;
        }
    }
    if (!resolved) {
        return std::unexpected(PgnDiagnostic{"illegal or malformed SAN move '" +
                                                 std::string{san} + "'",
                                             line, column});
    }
    return *resolved;
}

std::expected<PgnDocument, PgnDiagnostic> parse_pgn(std::string_view source) {
    if (source.size() > maximum_pgn_bytes) {
        return std::unexpected(PgnDiagnostic{"PGN input exceeds 1 MiB", 1U, 1U});
    }
    if (source.starts_with("\xef\xbb\xbf")) {
        source.remove_prefix(3U);
    }
    Cursor cursor{source};
    PgnDocument document;
    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> tag_locations;
    cursor.whitespace();
    while (cursor.peek() == '[') {
        const auto tag_line = cursor.line;
        const auto tag_column = cursor.column;
        cursor.take();
        cursor.whitespace();
        std::string name;
        while (!cursor.end() &&
               (std::isalnum(static_cast<unsigned char>(cursor.peek())) != 0 ||
                cursor.peek() == '_')) {
            name.push_back(cursor.take());
        }
        cursor.whitespace();
        if (name.empty() || cursor.take() != '"') {
            return std::unexpected(PgnDiagnostic{"malformed PGN tag", tag_line, tag_column});
        }
        std::string value;
        bool closed = false;
        while (!cursor.end()) {
            auto character = cursor.take();
            if (character == '"') {
                closed = true;
                break;
            }
            if (character == '\\') {
                if (cursor.end()) {
                    break;
                }
                character = cursor.take();
            }
            if (character == '\n' || character == '\r') {
                break;
            }
            value.push_back(character);
        }
        cursor.whitespace();
        if (!closed || cursor.take() != ']') {
            return std::unexpected(PgnDiagnostic{"unterminated PGN tag", tag_line,
                                                 tag_column});
        }
        if (std::ranges::any_of(document.tags, [&name](const auto& item) {
                return item.first == name;
            })) {
            return std::unexpected(PgnDiagnostic{"duplicate PGN tag '" + name + "'",
                                                 tag_line, tag_column});
        }
        document.tags.emplace_back(std::move(name), std::move(value));
        tag_locations.emplace_back(document.tags.back().first,
                                   std::pair{tag_line, tag_column});
        cursor.whitespace();
    }

    const auto tag_error = [&tag_locations](std::string_view name, std::string message) {
        const auto found = std::ranges::find_if(
            tag_locations, [name](const auto& location) { return location.first == name; });
        return found == tag_locations.end()
                   ? PgnDiagnostic{std::move(message), 1U, 1U}
                   : PgnDiagnostic{std::move(message), found->second.first,
                                   found->second.second};
    };

    if (const auto white = document.tag("White")) {
        document.settings.white_name = std::string{*white};
    }
    if (const auto black = document.tag("Black")) {
        document.settings.black_name = std::string{*black};
    }
    if (document.settings.white_name.empty() || document.settings.white_name.size() > 64U ||
        document.settings.black_name.empty() || document.settings.black_name.size() > 64U) {
        return std::unexpected(tag_error(
            document.settings.white_name.empty() ||
                    document.settings.white_name.size() > 64U
                ? "White" : "Black",
            "PGN player names must contain 1..64 bytes"));
    }
    if (const auto time = document.tag("TimeControl")) {
        const auto parsed = parse_time_control(*time);
        if (!parsed) {
            return std::unexpected(tag_error("TimeControl", "unsupported PGN TimeControl"));
        }
        document.settings.time_control = *parsed;
    }
    if (document.tag("SetUp") == std::optional<std::string_view>{"1"}) {
        const auto fen = document.tag("FEN");
        if (!fen) {
            return std::unexpected(tag_error("SetUp", "SetUp=1 requires a FEN tag"));
        }
        const auto position = Position::from_fen(*fen);
        if (!position) {
            return std::unexpected(tag_error(
                "FEN", "invalid FEN tag: " + position.error().message));
        }
        document.settings.initial_position = *position;
    } else if (document.tag("FEN")) {
        return std::unexpected(tag_error("FEN", "FEN requires SetUp=1"));
    }

    auto position = document.settings.initial_position;
    while (true) {
        cursor.whitespace();
        if (cursor.end()) {
            break;
        }
        if (cursor.peek() == '(' || cursor.peek() == ')') {
            return std::unexpected(error_at(cursor,
                "PGN variations are not supported; import a single main line"));
        }
        if (cursor.peek() == '{' || cursor.peek() == ';') {
            const auto brace = cursor.take() == '{';
            const auto comment_line = cursor.line;
            const auto comment_column = cursor.column;
            std::string comment;
            while (!cursor.end() && (brace ? cursor.peek() != '}' : cursor.peek() != '\n')) {
                comment.push_back(cursor.take());
            }
            if (brace && (cursor.end() || cursor.take() != '}')) {
                return std::unexpected(PgnDiagnostic{"unterminated PGN comment",
                                                     comment_line, comment_column});
            }
            if (!document.mainline.empty()) {
                auto& ply = document.mainline.back();
                if (const auto clock = parse_clock(comment)) {
                    ply.clock_remaining_milliseconds = *clock;
                }
                ply.comments.push_back(std::move(comment));
            } else {
                document.leading_comments.push_back(std::move(comment));
            }
            continue;
        }
        const auto token_line = cursor.line;
        const auto token_column = cursor.column;
        std::string token;
        while (!cursor.end() &&
               std::isspace(static_cast<unsigned char>(cursor.peek())) == 0 &&
               cursor.peek() != '{' && cursor.peek() != ';' && cursor.peek() != '(' &&
               cursor.peek() != ')') {
            token.push_back(cursor.take());
        }
        if (token.empty()) {
            continue;
        }
        if (token.front() == '$') {
            if (document.mainline.empty()) {
                return std::unexpected(PgnDiagnostic{"NAG precedes the first move",
                                                     token_line, token_column});
            }
            std::uint32_t nag = 0U;
            const auto number = std::string_view{token}.substr(1U);
            const auto [end, code] = std::from_chars(number.data(),
                                                     number.data() + number.size(), nag);
            if (code != std::errc{} || end != number.data() + number.size() || nag > 255U) {
                return std::unexpected(PgnDiagnostic{"malformed PGN NAG",
                                                     token_line, token_column});
            }
            document.mainline.back().nags.push_back(nag);
            continue;
        }
        if (is_move_number(token)) {
            continue;
        }
        const auto dot = token.rfind('.');
        if (dot != std::string::npos) {
            const auto prefix = std::string_view{token}.substr(0U, dot + 1U);
            if (is_move_number(prefix)) {
                token.erase(0U, dot + 1U);
                if (token.empty()) {
                    continue;
                }
            }
        }
        if (valid_result(token)) {
            document.result_token = token;
            cursor.whitespace();
            if (!cursor.end()) {
                return std::unexpected(error_at(cursor, "content follows the PGN result"));
            }
            break;
        }
        if (document.mainline.size() >= ChessMatch::maximum_plies) {
            return std::unexpected(PgnDiagnostic{"PGN exceeds 20000 plies",
                                                 token_line, token_column});
        }
        auto move = resolve_san(position, token, token_line, token_column);
        if (!move) {
            return std::unexpected(move.error());
        }
        const auto canonical = to_san(position, *move);
        if (!canonical) {
            return std::unexpected(PgnDiagnostic{canonical.error().message,
                                                 token_line, token_column});
        }
        PgnPly parsed_ply{*move, *canonical, {}, {}, std::nullopt,
                          token_line, token_column};
        if (const auto nag = symbolic_nag(token)) {
            parsed_ply.nags.push_back(*nag);
        }
        document.mainline.push_back(std::move(parsed_ply));
        if (const auto applied = position.apply(*move); !applied) {
            return std::unexpected(PgnDiagnostic{applied.error().message,
                                                 token_line, token_column});
        }
    }
    if (const auto header_result = document.tag("Result")) {
        if (!valid_result(*header_result)) {
            return std::unexpected(tag_error("Result", "invalid Result tag"));
        }
        if (document.result_token == "*") {
            document.result_token = std::string{*header_result};
        } else if (*header_result != document.result_token) {
            return std::unexpected(tag_error("Result", "Result tag and movetext disagree"));
        }
    }
    return document;
}

std::expected<ChessMatch, PgnDiagnostic>
import_pgn(PythonRuntime& runtime, std::string_view source) {
    auto parsed = parse_pgn(source);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto created = ChessMatch::create(runtime, parsed->settings);
    if (!created) {
        return std::unexpected(PgnDiagnostic{created.error().message, 1U, 1U});
    }
    auto match = std::move(*created);
    auto position = parsed->settings.initial_position;
    for (std::size_t index = 0U; index < parsed->mainline.size(); ++index) {
        const auto& ply = parsed->mainline[index];
        const auto moving = position.side_to_move();
        auto elapsed = std::int64_t{0};
        const auto before = match.remaining(moving);
        if (ply.clock_remaining_milliseconds && before) {
            elapsed = *before + parsed->settings.time_control.increment_milliseconds -
                      *ply.clock_remaining_milliseconds;
            if (elapsed < 0) {
                return std::unexpected(PgnDiagnostic{"clock annotation increases beyond increment",
                                                     ply.line, ply.column});
            }
        }
        const auto committed = match.submit(ply.move, elapsed);
        if (!committed) {
            return std::unexpected(PgnDiagnostic{committed.error().message,
                                                 ply.line, ply.column});
        }
        if (const auto applied = position.apply(ply.move); !applied) {
            return std::unexpected(PgnDiagnostic{applied.error().message,
                                                 ply.line, ply.column});
        }
    }
    const auto expected_token = match_result_token(match.result());
    if (parsed->result_token != "*" && match.result().terminal() &&
        parsed->result_token != expected_token) {
        return std::unexpected(PgnDiagnostic{"PGN result contradicts the final position", 1U, 1U});
    }
    if (parsed->result_token != "*" && !match.result().terminal()) {
        const auto termination_tag = parsed->tag("Termination").value_or("");
        if (parsed->result_token == "1/2-1/2") {
            if (termination_tag == "time forfeit") {
                const auto current_position = match.game().position();
                const auto loser = current_position ? current_position->side_to_move()
                                                    : Color::white;
                const auto remaining = match.remaining(loser);
                if (!current_position || !remaining) {
                    return std::unexpected(PgnDiagnostic{
                        "time-forfeit PGN requires a clocked active side", 1U, 1U});
                }
                const auto flagged = match.flag(loser, *remaining);
                if (!flagged || match.result().outcome != MatchOutcome::draw) {
                    return std::unexpected(PgnDiagnostic{
                        "time-forfeit PGN contradicts the available mating material", 1U, 1U});
                }
            } else if (termination_tag == "threefold repetition" ||
                       termination_tag == "fifty-move rule") {
                const auto claim_reason = termination_tag == "threefold repetition"
                                              ? MatchResultReason::threefold_repetition
                                              : MatchResultReason::fifty_move_rule;
                const auto claims = match.draw_claims();
                const auto found = std::ranges::find(claims, claim_reason,
                                                     &DrawClaim::reason);
                if (found == claims.end()) {
                    return std::unexpected(PgnDiagnostic{
                        "PGN termination names a draw that cannot be claimed", 1U, 1U});
                }
                const auto claimed = match.claim_draw(found->reason, found->intended_move);
                if (!claimed) {
                    return std::unexpected(PgnDiagnostic{claimed.error().message, 1U, 1U});
                }
            } else {
                const auto agreed = match.agree_draw();
                if (!agreed) {
                    return std::unexpected(PgnDiagnostic{agreed.error().message, 1U, 1U});
                }
            }
        } else {
            const auto loser = parsed->result_token == "1-0" ? Color::black : Color::white;
            if (termination_tag == "time forfeit") {
                const auto current_position = match.game().position();
                const auto remaining = match.remaining(loser);
                if (!current_position || current_position->side_to_move() != loser ||
                    !remaining) {
                    return std::unexpected(PgnDiagnostic{
                        "time-forfeit PGN requires a clocked active losing side", 1U, 1U});
                }
                const auto flagged = match.flag(loser, *remaining);
                if (!flagged) {
                    return std::unexpected(PgnDiagnostic{flagged.error().message, 1U, 1U});
                }
            } else {
                const auto resigned = match.resign(loser);
                if (!resigned) {
                    return std::unexpected(PgnDiagnostic{resigned.error().message, 1U, 1U});
                }
            }
        }
    }
    return match;
}

std::string export_pgn(const ChessMatch& match, std::string_view event,
                       std::string_view site) {
    const auto result = match_result_token(match.result());
    std::ostringstream output;
    const auto tag = [&output](std::string_view name, std::string_view value) {
        output << '[' << name << " \"" << escaped_tag(value) << "\"]\n";
    };
    tag("Event", event);
    tag("Site", site);
    tag("Date", "????.??.??");
    tag("Round", "-");
    tag("White", match.settings().white_name);
    tag("Black", match.settings().black_name);
    tag("Result", result);
    if (match.settings().time_control.clocked()) {
        tag("TimeControl", std::to_string(*match.settings().time_control.base_milliseconds / 1'000) +
                               "+" + std::to_string(
                                   match.settings().time_control.increment_milliseconds / 1'000));
    } else {
        tag("TimeControl", "-");
    }
    tag("Termination", termination(match.result()));
    if (match.settings().initial_position.to_fen() != Position::initial().to_fen()) {
        tag("SetUp", "1");
        tag("FEN", match.settings().initial_position.to_fen());
    }
    output << '\n';

    auto position = match.settings().initial_position;
    std::size_t line_width = 0U;
    const auto emit = [&output, &line_width](std::string token) {
        if (line_width != 0U && line_width + token.size() + 1U > 88U) {
            output << '\n';
            line_width = 0U;
        }
        if (line_width != 0U) {
            output << ' ';
            ++line_width;
        }
        output << token;
        line_width += token.size();
    };
    const auto history = match.history();
    for (std::size_t index = 0U; index < history.size(); ++index) {
        if (position.side_to_move() == Color::white) {
            emit(std::to_string(position.fullmove_number()) + ".");
        } else if (index == 0U) {
            emit(std::to_string(position.fullmove_number()) + "...");
        }
        const auto san = to_san(position, history[index].move);
        emit(san ? *san : to_uci(history[index].move));
        const auto remaining = position.side_to_move() == Color::white
                                   ? history[index].white_remaining_milliseconds
                                   : history[index].black_remaining_milliseconds;
        if (remaining) {
            emit("{[%clk " + clock_text(*remaining) + "]}");
        }
        if (const auto applied = position.apply(history[index].move); !applied) {
            break;
        }
    }
    emit(result);
    output << '\n';
    return output.str();
}

} // namespace ludus::chess
