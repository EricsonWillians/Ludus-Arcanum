#pragma once

#include "ludus/core/binary.hpp"
#include "ludus/core/diagnostic.hpp"
#include "ludus/core/id.hpp"
#include "ludus/rules/game_state.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ludus {

/// Operations accepted in an authoring graph and emitted in native movement bytecode.
enum class RuleOpcode : std::uint8_t {
    traverse_rays = 1,
    traverse_jumps = 2,
    until_blocked = 3,
    emit_empty = 4,
    emit_enemy_capture = 5,
    end = 255,
};

struct RuleNode {
    RuleOpcode opcode{RuleOpcode::end};
    std::uint32_t operand{0};

    auto operator<=>(const RuleNode&) const = default;
};

/// Immutable authoring product produced by the Python DSL or a native package loader.
struct MovementRuleGraph {
    std::vector<DirectionId> directions;
    std::vector<RuleNode> nodes;

    auto operator<=>(const MovementRuleGraph&) const = default;
};

struct RuleInstruction {
    RuleOpcode opcode{RuleOpcode::end};
    std::uint32_t operand{0};

    auto operator<=>(const RuleInstruction&) const = default;
};

/// Validated, compact program consumed only by the C++ evaluator.
class RuleProgram {
  public:
    static constexpr std::uint32_t bytecode_version = 1U;
    static constexpr std::size_t max_instructions = 32U;
    static constexpr std::size_t max_directions = 64U;

    [[nodiscard]] std::span<const DirectionId> directions() const noexcept {
        return directions_;
    }
    [[nodiscard]] std::span<const RuleInstruction> instructions() const noexcept {
        return instructions_;
    }

    [[nodiscard]] std::vector<std::byte> canonical_bytes() const;
    [[nodiscard]] std::uint64_t canonical_hash() const;
    [[nodiscard]] static std::expected<RuleProgram, Diagnostic>
    from_canonical_bytes(std::span<const std::byte> bytes);

    auto operator<=>(const RuleProgram&) const = default;

  private:
    std::vector<DirectionId> directions_;
    std::vector<RuleInstruction> instructions_;

    friend std::expected<RuleProgram, Diagnostic>
    lower_movement_rule(const MovementRuleGraph& graph);
};

struct MoveCandidate {
    SpaceId destination;
    std::optional<EntityId> capture;

    auto operator<=>(const MoveCandidate&) const = default;
};

/// Validates and optimizes an authoring graph into bytecode.
[[nodiscard]] std::expected<RuleProgram, Diagnostic>
lower_movement_rule(const MovementRuleGraph& graph);

/// Evaluates rays, jumps, blocking, occupancy, and enemy ownership in one native batch.
[[nodiscard]] std::expected<std::vector<MoveCandidate>, Diagnostic>
evaluate_movement(const GameState& state, EntityId actor, const RuleProgram& program);

} // namespace ludus
