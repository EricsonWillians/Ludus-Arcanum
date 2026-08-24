#pragma once

#include "ludus/chess/chess.hpp"
#include "ludus/python/runtime.hpp"
#include "ludus/rules/event.hpp"
#include "ludus/rules/session.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ludus::chess {

struct ChessRuntimeData;

/// External orthodox-chess package adapter backed by an authoritative GameSession.
/// The PythonRuntime passed to create() must outlive this object.
class ChessGame {
  public:
    ChessGame(const ChessGame&) = delete;
    ChessGame& operator=(const ChessGame&) = delete;
    ChessGame(ChessGame&&) noexcept = default;
    ChessGame& operator=(ChessGame&&) noexcept = default;

    [[nodiscard]] static std::expected<ChessGame, Diagnostic>
    create(PythonRuntime& python, Position initial = Position::initial(),
           std::string_view rule_module = "orthodox_chess.rules");

    [[nodiscard]] const GameSession& session() const noexcept { return session_; }
    [[nodiscard]] const Position& initial_position() const noexcept { return initial_position_; }
    [[nodiscard]] std::expected<Position, Diagnostic> position() const;
    [[nodiscard]] std::expected<std::vector<ChessMove>, Diagnostic> legal_moves() const;
    [[nodiscard]] std::expected<PositionStatus, Diagnostic> status() const;

    [[nodiscard]] std::expected<EventBatch, Diagnostic> submit(ChessMove move);
    [[nodiscard]] std::expected<EventBatch, Diagnostic> submit_uci(std::string_view uci);
    [[nodiscard]] std::expected<void, Diagnostic> undo();
    [[nodiscard]] std::expected<void, Diagnostic> redo();
    [[nodiscard]] std::expected<bool, Diagnostic> reload_rules();

    [[nodiscard]] std::span<const ChessMove> move_history() const noexcept {
        return std::span<const ChessMove>{history_}.first(history_cursor_);
    }
    [[nodiscard]] std::uint64_t state_hash() const { return session_.state_hash(); }
    [[nodiscard]] std::expected<std::uint64_t, Diagnostic> replayed_state_hash() const {
        return session_.replayed_state_hash();
    }

  private:
    ChessGame(std::shared_ptr<ChessRuntimeData> runtime_data, GameSession session,
              Position initial_position)
        : runtime_data_(std::move(runtime_data)), session_(std::move(session)),
          initial_position_(std::move(initial_position)) {}

    std::shared_ptr<ChessRuntimeData> runtime_data_;
    GameSession session_;
    Position initial_position_;
    std::vector<ChessMove> history_;
    std::size_t history_cursor_{0U};
};

} // namespace ludus::chess
