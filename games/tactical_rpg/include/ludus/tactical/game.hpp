#pragma once

#include "ludus/python/runtime.hpp"
#include "ludus/rules/session.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ludus::tactical {

inline constexpr PlayerId vanguard_player{0U, 1U};
inline constexpr PlayerId raiders_player{1U, 1U};

struct AxialCoord {
    int q{0};
    int r{0};

    auto operator<=>(const AxialCoord&) const = default;
};

enum class Ability : std::uint32_t {
    quick_shot = 1U,
    focused_shot = 2U,
    venom_shot = 3U,
    shield_bash = 4U,
    guard = 5U,
    arc_bolt = 6U,
    ward = 7U,
    crush = 8U,
    bulwark = 9U,
    dash = 10U,
    ambush = 11U,
    blight_bolt = 12U,
    drain = 13U,

    // Source-compatibility aliases for scenario-version-1 callers.
    basic = quick_shot,
    power = focused_shot,
    venom = venom_shot,
};

struct TacticalRuntimeData;

/// External tactical-RPG adapter. All genre semantics remain package-local.
class TacticalGame {
  public:
    TacticalGame(const TacticalGame&) = delete;
    TacticalGame& operator=(const TacticalGame&) = delete;
    TacticalGame(TacticalGame&&) noexcept = default;
    TacticalGame& operator=(TacticalGame&&) noexcept = default;

    [[nodiscard]] static std::expected<TacticalGame, Diagnostic>
    create(PythonRuntime& python, std::uint64_t seed = 0x544143544943414CULL,
           std::string_view rule_module = "tactical_rpg.rules");
    [[nodiscard]] static std::expected<TacticalGame, Diagnostic>
    restore(PythonRuntime& python, GameSession session,
            std::string_view rule_module = "tactical_rpg.rules");

    [[nodiscard]] const GameSession& session() const noexcept { return session_; }
    [[nodiscard]] std::vector<ActionIntent> legal_actions(PlayerId player) const;
    [[nodiscard]] std::expected<EventBatch, Diagnostic>
    move_unit(EntityId actor, SpaceId destination);
    [[nodiscard]] std::expected<EventBatch, Diagnostic>
    begin_attack(EntityId actor, EntityId target);
    [[nodiscard]] std::expected<EventBatch, Diagnostic> choose(Ability ability);
    [[nodiscard]] std::expected<EventBatch, Diagnostic>
    use_ability(EntityId actor, Ability ability, EntityId target);
    [[nodiscard]] std::expected<EventBatch, Diagnostic> end_activation();
    [[nodiscard]] std::expected<EventBatch, Diagnostic> submit_token(std::uint64_t token);
    [[nodiscard]] std::expected<std::vector<EventBatch>, Diagnostic>
    submit_player_token(std::uint64_t token);
    [[nodiscard]] std::expected<std::vector<EventBatch>, Diagnostic> run_ai();
    [[nodiscard]] std::expected<void, Diagnostic> undo();
    [[nodiscard]] std::expected<void, Diagnostic> redo();
    [[nodiscard]] std::expected<void, Diagnostic> undo_player_decision();
    [[nodiscard]] std::expected<void, Diagnostic> redo_player_decision();
    void set_hot_seat(bool enabled) noexcept { ai_enabled_ = !enabled; }
    [[nodiscard]] bool hot_seat() const noexcept { return !ai_enabled_; }
    [[nodiscard]] std::expected<PlayerId, Diagnostic> active_player() const;
    [[nodiscard]] std::span<const std::uint64_t> action_history() const noexcept {
        return std::span<const std::uint64_t>{history_}.first(history_cursor_);
    }

    [[nodiscard]] std::optional<SpaceId> space(AxialCoord coordinate) const;
    [[nodiscard]] std::expected<EntityId, Diagnostic>
    entity_named(std::string_view kind) const;
    [[nodiscard]] SpaceId inventory(PlayerId player) const noexcept;
    [[nodiscard]] SpaceId discard() const noexcept;
    [[nodiscard]] std::uint32_t scenario_version() const noexcept;
    [[nodiscard]] std::uint64_t state_hash() const { return session_.state_hash(); }
    [[nodiscard]] std::expected<std::uint64_t, Diagnostic> replayed_state_hash() const {
        return session_.replayed_state_hash();
    }

  private:
    TacticalGame(std::shared_ptr<TacticalRuntimeData> runtime, GameSession session)
        : runtime_(std::move(runtime)), session_(std::move(session)) {}

    void record_action(std::uint64_t token);

    std::shared_ptr<TacticalRuntimeData> runtime_;
    GameSession session_;
    std::vector<std::uint64_t> history_;
    std::vector<std::uint64_t> history_groups_;
    std::size_t history_cursor_{0U};
    std::uint64_t next_history_group_{0U};
    std::uint64_t recording_group_{0U};
    bool ai_enabled_{true};
};

[[nodiscard]] int hex_distance(AxialCoord first, AxialCoord second) noexcept;
[[nodiscard]] std::uint64_t encode_action_token(const ActionIntent& intent);
[[nodiscard]] std::uint64_t encode_choice_token(std::uint64_t choice_id,
                                                std::uint32_t option_id);

} // namespace ludus::tactical
