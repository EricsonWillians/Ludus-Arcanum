#pragma once

#include "ludus/chess/game.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ludus::chess {

struct TimeControl {
    std::optional<std::int64_t> base_milliseconds;
    std::int64_t increment_milliseconds{0};

    [[nodiscard]] bool clocked() const noexcept { return base_milliseconds.has_value(); }
    auto operator<=>(const TimeControl&) const = default;
};

struct ChessMatchSettings {
    std::string white_name{"Ivory"};
    std::string black_name{"Iron"};
    Position initial_position{Position::initial()};
    TimeControl time_control;

};

enum class MatchOutcome : std::uint8_t { ongoing, white_wins, black_wins, draw };

enum class MatchResultReason : std::uint8_t {
    none,
    checkmate,
    stalemate,
    resignation,
    timeout,
    agreed_draw,
    threefold_repetition,
    fifty_move_rule,
    fivefold_repetition,
    seventy_five_move_rule,
    insufficient_material,
};

struct ChessMatchResult {
    MatchOutcome outcome{MatchOutcome::ongoing};
    MatchResultReason reason{MatchResultReason::none};
    std::optional<Color> actor;
    std::optional<ChessMove> intended_move;

    [[nodiscard]] bool terminal() const noexcept { return outcome != MatchOutcome::ongoing; }
    auto operator<=>(const ChessMatchResult&) const = default;
};

struct TimedPly {
    ChessMove move;
    std::int64_t elapsed_milliseconds{0};
    std::optional<std::int64_t> white_remaining_milliseconds;
    std::optional<std::int64_t> black_remaining_milliseconds;

    auto operator<=>(const TimedPly&) const = default;
};

struct DrawClaim {
    MatchResultReason reason{MatchResultReason::none};
    std::optional<ChessMove> intended_move;

    auto operator<=>(const DrawClaim&) const = default;
};

struct MatchUpdate {
    std::optional<EventBatch> events;
    ChessMatchResult result;
};

/// Deterministic local-match policy around the unchanged orthodox ChessGame.
class ChessMatch {
  public:
    static constexpr std::size_t maximum_plies = 20'000U;
    static constexpr std::int64_t maximum_base_milliseconds = 86'400'000;
    static constexpr std::int64_t maximum_increment_milliseconds = 3'600'000;

    ChessMatch(const ChessMatch&) = delete;
    ChessMatch& operator=(const ChessMatch&) = delete;
    ChessMatch(ChessMatch&&) noexcept = default;
    ChessMatch& operator=(ChessMatch&&) noexcept = default;

    [[nodiscard]] static std::expected<ChessMatch, Diagnostic>
    create(PythonRuntime& runtime, ChessMatchSettings settings = {});
    [[nodiscard]] static std::expected<ChessMatch, Diagnostic>
    load(PythonRuntime& runtime, std::span<const std::byte> archive);

    [[nodiscard]] const ChessGame& game() const noexcept { return game_; }
    [[nodiscard]] const ChessMatchSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const ChessMatchResult& result() const noexcept { return result_; }
    [[nodiscard]] std::optional<std::int64_t> result_elapsed_milliseconds() const noexcept {
        return terminal_elapsed_milliseconds_;
    }
    [[nodiscard]] std::span<const TimedPly> history() const noexcept {
        return std::span<const TimedPly>{history_}.first(history_cursor_);
    }
    [[nodiscard]] std::size_t history_cursor() const noexcept { return history_cursor_; }
    [[nodiscard]] std::size_t history_size() const noexcept { return history_.size(); }
    [[nodiscard]] std::optional<std::int64_t> remaining(Color color) const noexcept;
    [[nodiscard]] std::optional<Color> draw_offer() const noexcept { return draw_offer_; }
    [[nodiscard]] std::vector<DrawClaim> draw_claims() const;

    [[nodiscard]] std::expected<MatchUpdate, Diagnostic>
    submit(ChessMove move, std::int64_t elapsed_milliseconds = 0);
    [[nodiscard]] std::expected<MatchUpdate, Diagnostic>
    submit_uci(std::string_view uci, std::int64_t elapsed_milliseconds = 0);
    [[nodiscard]] std::expected<MatchUpdate, Diagnostic>
    flag(Color color, std::int64_t elapsed_milliseconds);
    [[nodiscard]] std::expected<void, Diagnostic> resign(Color color);
    [[nodiscard]] std::expected<void, Diagnostic> agree_draw();
    [[nodiscard]] std::expected<void, Diagnostic> offer_draw(Color color);
    [[nodiscard]] std::expected<void, Diagnostic> decline_draw(Color color);
    [[nodiscard]] std::expected<void, Diagnostic>
    claim_draw(MatchResultReason reason, std::optional<ChessMove> intended_move = std::nullopt);
    [[nodiscard]] std::expected<void, Diagnostic> undo();
    [[nodiscard]] std::expected<void, Diagnostic> redo();

    [[nodiscard]] std::vector<std::byte> save() const;
    [[nodiscard]] std::uint64_t match_hash() const;

  private:
    ChessMatch(ChessMatchSettings settings, ChessGame game);

    [[nodiscard]] std::expected<void, Diagnostic> validate_elapsed(std::int64_t value) const;
    void restore_clock_at_cursor();
    void refresh_automatic_result();
    [[nodiscard]] std::size_t repetition_count(const Position& position,
                                               bool include_candidate) const;

    ChessMatchSettings settings_;
    ChessGame game_;
    std::vector<TimedPly> history_;
    std::size_t history_cursor_{0U};
    std::optional<std::int64_t> white_remaining_;
    std::optional<std::int64_t> black_remaining_;
    ChessMatchResult result_;
    std::optional<Color> draw_offer_;
    std::optional<std::int64_t> terminal_elapsed_milliseconds_;
    std::optional<ChessMatchResult> undone_manual_result_;
    std::optional<std::int64_t> undone_manual_elapsed_milliseconds_;
};

/// Stable native archive facade; authoritative position hashes remain ChessGame-owned.
struct ChessMatchArchive {
    static constexpr std::uint32_t current_version = 2U;

    [[nodiscard]] static std::vector<std::byte> save(const ChessMatch& match) {
        return match.save();
    }
    [[nodiscard]] static std::expected<ChessMatch, Diagnostic>
    load(PythonRuntime& runtime, std::span<const std::byte> archive) {
        return ChessMatch::load(runtime, archive);
    }
};

[[nodiscard]] bool insufficient_material(const Position& position) noexcept;
[[nodiscard]] bool has_possible_mating_material(const Position& position,
                                                Color color) noexcept;
[[nodiscard]] std::string match_result_token(const ChessMatchResult& result);
[[nodiscard]] std::string match_result_description(const ChessMatchResult& result);

} // namespace ludus::chess
