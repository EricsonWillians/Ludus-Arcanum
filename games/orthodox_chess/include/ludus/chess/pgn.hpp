#pragma once

#include "ludus/chess/match.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ludus::chess {

struct PgnDiagnostic {
    std::string message;
    std::size_t line{1U};
    std::size_t column{1U};
};

struct PgnPly {
    ChessMove move;
    std::string san;
    std::vector<std::string> comments;
    std::vector<std::uint32_t> nags;
    std::optional<std::int64_t> clock_remaining_milliseconds;
    std::size_t line{1U};
    std::size_t column{1U};
};

struct PgnDocument {
    std::vector<std::pair<std::string, std::string>> tags;
    ChessMatchSettings settings;
    std::vector<std::string> leading_comments;
    std::vector<PgnPly> mainline;
    std::string result_token{"*"};

    [[nodiscard]] std::optional<std::string_view> tag(std::string_view name) const noexcept;
};

[[nodiscard]] std::expected<ChessMove, PgnDiagnostic>
resolve_san(const Position& position, std::string_view san,
            std::size_t line = 1U, std::size_t column = 1U);

[[nodiscard]] std::expected<PgnDocument, PgnDiagnostic>
parse_pgn(std::string_view source);

[[nodiscard]] std::expected<ChessMatch, PgnDiagnostic>
import_pgn(PythonRuntime& runtime, std::string_view source);

[[nodiscard]] std::string export_pgn(const ChessMatch& match,
                                     std::string_view event = "Casual Game",
                                     std::string_view site = "Local");

} // namespace ludus::chess
