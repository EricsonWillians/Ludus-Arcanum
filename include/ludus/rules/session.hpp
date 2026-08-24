#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/rules/action.hpp"
#include "ludus/rules/event.hpp"
#include "ludus/rules/game_state.hpp"
#include "ludus/rules/random.hpp"
#include "ludus/rules/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <vector>

namespace ludus {

class GameSession {
  public:
    static constexpr std::uint32_t archive_version = 2U;
    static constexpr std::size_t checkpoint_interval = 32U;
    static constexpr std::size_t max_history_entries = 1'000'000U;
    static constexpr std::size_t max_legal_actions = 65'536U;

    explicit GameSession(GameState initial_state, std::uint64_t seed);

    [[nodiscard]] const GameState& state() const noexcept { return state_; }
    [[nodiscard]] const DeterministicRandom& random() const noexcept { return random_; }
    [[nodiscard]] std::uint64_t state_hash() const;
    [[nodiscard]] std::size_t history_cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t history_size() const noexcept { return history_.size(); }

    [[nodiscard]] std::expected<void, Diagnostic>
    define_action(ActionDefinition definition, ActionValidator validator,
                  ActionResolver resolver, ActionEnumerator enumerator = {});
    [[nodiscard]] std::expected<void, Diagnostic> validate(const ActionIntent& intent) const;
    [[nodiscard]] std::vector<ActionIntent> legal_actions(PlayerId player) const;
    [[nodiscard]] std::expected<EventBatch, Diagnostic> submit(const ActionIntent& intent);

    [[nodiscard]] std::expected<void, Diagnostic> undo();
    [[nodiscard]] std::expected<void, Diagnostic> redo();
    [[nodiscard]] std::expected<std::uint64_t, Diagnostic> replayed_state_hash() const;

    [[nodiscard]] std::vector<std::byte> save() const;
    [[nodiscard]] static std::expected<GameSession, Diagnostic>
    load(std::span<const std::byte> archive);

  private:
    struct RegisteredAction {
        ActionDefinition definition;
        ActionValidator validator;
        ActionResolver resolver;
        ActionEnumerator enumerator;
    };

    struct HistoryEntry {
        ActionIntent intent;
        EventBatch batch;
        std::vector<detail::StatePatch> patches;
        RandomSnapshot random_before;
        RandomSnapshot random_after;
    };

    struct Checkpoint {
        std::size_t history_index{0};
        std::vector<std::byte> state;
        RandomSnapshot random;
    };

    static void apply_patch(GameState& state, const detail::StatePatch& patch, bool forward);
    [[nodiscard]] static std::expected<detail::StatePatch, Diagnostic>
    apply_event(GameState& state, const Event& event);
    void truncate_redo();
    void maybe_checkpoint();

    GameState state_;
    DeterministicRandom random_;
    std::vector<std::byte> initial_state_;
    std::map<ActionTypeId, RegisteredAction> actions_;
    std::vector<HistoryEntry> history_;
    std::size_t cursor_{0};
    std::uint64_t next_event_sequence_{1};
    std::vector<Checkpoint> checkpoints_;
};

} // namespace ludus
