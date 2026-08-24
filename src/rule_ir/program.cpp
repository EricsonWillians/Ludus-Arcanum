#include "ludus/rule_ir/program.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

namespace ludus {
namespace {

constexpr std::string_view program_magic = "LUDUS-RULE-IR";

Diagnostic invalid_graph(std::string message) {
    return Diagnostic{DiagnosticCode::validation_failed, std::move(message), {}};
}

bool is_known_opcode(RuleOpcode opcode) noexcept {
    switch (opcode) {
    case RuleOpcode::traverse_rays:
    case RuleOpcode::traverse_jumps:
    case RuleOpcode::until_blocked:
    case RuleOpcode::emit_empty:
    case RuleOpcode::emit_enemy_capture:
    case RuleOpcode::end:
        return true;
    }
    return false;
}

struct DecodedProgram {
    bool rays{false};
    bool jumps{false};
    bool until_blocked{false};
    bool emit_empty{false};
    bool capture_enemy{false};
    std::uint32_t distance{0};
};

std::expected<DecodedProgram, Diagnostic> decode_program(const RuleProgram& program) {
    DecodedProgram result;
    const auto instructions = program.instructions();
    if (instructions.empty()) {
        return std::unexpected(invalid_graph("rule bytecode is incomplete"));
    }

    std::size_t cursor = 0U;
    const auto traversal = instructions[cursor++];
    if (traversal.opcode == RuleOpcode::traverse_rays) {
        result.rays = true;
        result.distance = traversal.operand;
        if (cursor >= instructions.size() ||
            instructions[cursor] != RuleInstruction{RuleOpcode::until_blocked, 0U}) {
            return std::unexpected(invalid_graph("ray bytecode must stop at blockers"));
        }
        result.until_blocked = true;
        ++cursor;
    } else if (traversal.opcode == RuleOpcode::traverse_jumps && traversal.operand > 0U) {
        result.jumps = true;
        result.distance = traversal.operand;
    } else {
        return std::unexpected(invalid_graph("rule bytecode traversal is invalid"));
    }

    if (cursor < instructions.size() && instructions[cursor].opcode == RuleOpcode::emit_empty) {
        if (instructions[cursor].operand != 0U) {
            return std::unexpected(invalid_graph("allow_empty bytecode is malformed"));
        }
        result.emit_empty = true;
        ++cursor;
    }
    if (cursor < instructions.size() &&
        instructions[cursor].opcode == RuleOpcode::emit_enemy_capture) {
        if (instructions[cursor].operand != 0U) {
            return std::unexpected(invalid_graph("capture_enemy bytecode is malformed"));
        }
        result.capture_enemy = true;
        ++cursor;
    }
    if (!result.emit_empty && !result.capture_enemy) {
        return std::unexpected(invalid_graph("rule bytecode emits no movement candidates"));
    }
    if (cursor >= instructions.size() ||
        instructions[cursor] != RuleInstruction{RuleOpcode::end, 0U}) {
        return std::unexpected(invalid_graph("rule bytecode is not in canonical order"));
    }
    ++cursor;
    if (cursor != instructions.size()) {
        return std::unexpected(invalid_graph("rule bytecode contains data after end"));
    }
    return result;
}

std::vector<std::vector<EntityId>> build_occupancy(const GameState& state) {
    std::vector<std::vector<EntityId>> result(state.topology().spaces().size());
    for (const auto entity : state.entities().entities()) {
        const auto snapshot = state.entities().snapshot(entity);
        if (snapshot && snapshot->location && state.topology().contains(*snapshot->location)) {
            result[snapshot->location->index()].push_back(entity);
        }
    }
    return result;
}

struct LandingResult {
    bool blocked{false};
    std::optional<MoveCandidate> candidate;
};

LandingResult inspect_landing(const GameState& state,
                              const std::vector<std::vector<EntityId>>& occupancy,
                              const EntitySnapshot& actor, SpaceId destination,
                              const DecodedProgram& program) {
    const auto& occupants = occupancy[destination.index()];
    if (occupants.empty()) {
        return LandingResult{false, program.emit_empty
                                        ? std::optional<MoveCandidate>{
                                              MoveCandidate{destination, std::nullopt}}
                                        : std::nullopt};
    }

    std::optional<EntityId> first_enemy;
    bool has_friendly = false;
    for (const auto occupant : occupants) {
        if (occupant == actor.id) {
            has_friendly = true;
            continue;
        }
        const auto target = state.entities().snapshot(occupant);
        if (!target) {
            continue;
        }
        if (target->owner == actor.owner) {
            has_friendly = true;
        } else if (!first_enemy) {
            first_enemy = occupant;
        }
    }

    if (!has_friendly && first_enemy && program.capture_enemy) {
        return LandingResult{true, MoveCandidate{destination, first_enemy}};
    }
    return LandingResult{true, std::nullopt};
}

std::vector<SpaceId> advance_direction(const Topology& topology, std::span<const SpaceId> frontier,
                                       DirectionId direction) {
    std::vector<SpaceId> next;
    for (const auto from : frontier) {
        for (const auto& link : topology.outgoing(from)) {
            if (link.direction == direction) {
                next.push_back(link.to);
            }
        }
    }
    std::ranges::sort(next);
    const auto unique_end = std::ranges::unique(next).begin();
    next.erase(unique_end, next.end());
    return next;
}

} // namespace

std::expected<RuleProgram, Diagnostic> lower_movement_rule(const MovementRuleGraph& graph) {
    if (graph.directions.empty() || graph.directions.size() > RuleProgram::max_directions) {
        return std::unexpected(invalid_graph("movement rule direction count is invalid"));
    }
    if (graph.nodes.empty() || graph.nodes.size() + 1U > RuleProgram::max_instructions) {
        return std::unexpected(invalid_graph("movement rule instruction count is invalid"));
    }

    RuleProgram result;
    result.directions_ = graph.directions;
    if (std::ranges::any_of(result.directions_, [](DirectionId direction) {
            return !direction.valid();
        })) {
        return std::unexpected(invalid_graph("movement rule contains an invalid direction"));
    }
    std::ranges::sort(result.directions_);
    const auto unique_end = std::ranges::unique(result.directions_).begin();
    result.directions_.erase(unique_end, result.directions_.end());

    std::optional<RuleInstruction> traversal;
    bool until_seen = false;
    bool empty_seen = false;
    bool capture_seen = false;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        const auto& node = graph.nodes[index];
        if (!is_known_opcode(node.opcode) || node.opcode == RuleOpcode::end) {
            return std::unexpected(invalid_graph("movement graph contains an unknown operation"));
        }
        switch (node.opcode) {
        case RuleOpcode::traverse_rays:
        case RuleOpcode::traverse_jumps:
            if (traversal || index != 0U) {
                return std::unexpected(
                    invalid_graph("movement traversal must be the first and only traversal"));
            }
            if (node.opcode == RuleOpcode::traverse_jumps && node.operand == 0U) {
                return std::unexpected(invalid_graph("jump distance must be positive"));
            }
            traversal = RuleInstruction{node.opcode, node.operand};
            break;
        case RuleOpcode::until_blocked:
            if (until_seen || node.operand != 0U) {
                return std::unexpected(invalid_graph("until_blocked is duplicated or malformed"));
            }
            until_seen = true;
            break;
        case RuleOpcode::emit_empty:
            if (empty_seen || node.operand != 0U) {
                return std::unexpected(invalid_graph("allow_empty is duplicated or malformed"));
            }
            empty_seen = true;
            break;
        case RuleOpcode::emit_enemy_capture:
            if (capture_seen || node.operand != 0U) {
                return std::unexpected(invalid_graph("capture_enemy is duplicated or malformed"));
            }
            capture_seen = true;
            break;
        case RuleOpcode::end:
            break;
        }
    }

    if (!traversal) {
        return std::unexpected(invalid_graph("movement rule requires one traversal"));
    }
    if (traversal->opcode == RuleOpcode::traverse_rays && !until_seen) {
        return std::unexpected(invalid_graph("ray bytecode must stop at blockers"));
    }
    if (traversal->opcode == RuleOpcode::traverse_jumps && until_seen) {
        return std::unexpected(invalid_graph("jump bytecode cannot use until_blocked"));
    }
    if (!empty_seen && !capture_seen) {
        return std::unexpected(invalid_graph("movement rule emits no movement candidates"));
    }

    result.instructions_.push_back(*traversal);
    if (until_seen) {
        result.instructions_.push_back(RuleInstruction{RuleOpcode::until_blocked, 0U});
    }
    if (empty_seen) {
        result.instructions_.push_back(RuleInstruction{RuleOpcode::emit_empty, 0U});
    }
    if (capture_seen) {
        result.instructions_.push_back(RuleInstruction{RuleOpcode::emit_enemy_capture, 0U});
    }
    result.instructions_.push_back(RuleInstruction{RuleOpcode::end, 0U});

    if (const auto checked = decode_program(result); !checked) {
        return std::unexpected(checked.error());
    }
    return result;
}

std::vector<std::byte> RuleProgram::canonical_bytes() const {
    BinaryWriter writer;
    writer.string(program_magic);
    writer.u32(bytecode_version);
    writer.u64(static_cast<std::uint64_t>(directions_.size()));
    for (const auto direction : directions_) {
        writer.u32(direction.value());
    }
    writer.u64(static_cast<std::uint64_t>(instructions_.size()));
    for (const auto& instruction : instructions_) {
        writer.u8(static_cast<std::uint8_t>(instruction.opcode));
        writer.u32(instruction.operand);
    }
    return std::move(writer).take();
}

std::uint64_t RuleProgram::canonical_hash() const {
    const auto bytes = canonical_bytes();
    return ludus::canonical_hash(bytes);
}

std::expected<RuleProgram, Diagnostic>
RuleProgram::from_canonical_bytes(std::span<const std::byte> bytes) {
    BinaryReader reader{bytes};
    if (reader.string() != program_magic || reader.u32() != bytecode_version) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "unsupported or invalid rule bytecode header", {}});
    }
    RuleProgram result;
    const auto direction_count = reader.u64();
    if (direction_count == 0U || direction_count > max_directions) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "serialized rule direction count is invalid", {}});
    }
    for (std::uint64_t index = 0; index < direction_count && reader.ok(); ++index) {
        result.directions_.emplace_back(reader.u32());
    }
    if (std::ranges::any_of(result.directions_, [](DirectionId direction) {
            return !direction.valid();
        }) ||
        !std::ranges::is_sorted(result.directions_) ||
        std::ranges::adjacent_find(result.directions_) != result.directions_.end()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "serialized rule directions are not canonical", {}});
    }

    const auto instruction_count = reader.u64();
    if (instruction_count == 0U || instruction_count > max_instructions) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "serialized rule instruction count is invalid", {}});
    }
    for (std::uint64_t index = 0; index < instruction_count && reader.ok(); ++index) {
        const auto opcode = static_cast<RuleOpcode>(reader.u8());
        const auto operand = reader.u32();
        if (!is_known_opcode(opcode)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "serialized rule opcode is invalid", {}});
        }
        result.instructions_.push_back(RuleInstruction{opcode, operand});
    }
    if (!reader.ok() || !reader.at_end()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          reader.ok() ? "trailing rule bytecode data"
                                                      : std::string{reader.error()},
                                          {}});
    }
    if (const auto checked = decode_program(result); !checked) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          checked.error().message, {}});
    }
    return result;
}

std::expected<std::vector<MoveCandidate>, Diagnostic>
evaluate_movement(const GameState& state, EntityId actor_id, const RuleProgram& program) {
    const auto actor = state.entities().snapshot(actor_id);
    if (!actor || !actor->location || !state.topology().contains(*actor->location)) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_handle,
                                          "movement actor has no valid logical location", {}});
    }
    for (const auto direction : program.directions()) {
        if (direction.value() > state.symbols().directions.size()) {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              "movement program references an unknown direction", {}});
        }
    }
    const auto decoded = decode_program(program);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    const auto occupancy = build_occupancy(state);
    std::vector<MoveCandidate> candidates;
    for (const auto direction : program.directions()) {
        std::vector<SpaceId> frontier{*actor->location};
        const auto step_limit = decoded->rays
                                    ? (decoded->distance == 0U
                                           ? state.topology().spaces().size()
                                           : static_cast<std::size_t>(decoded->distance))
                                    : static_cast<std::size_t>(decoded->distance);
        std::vector<bool> visited(state.topology().spaces().size(), false);
        visited[actor->location->index()] = true;
        for (std::size_t step = 0; step < step_limit && !frontier.empty(); ++step) {
            auto next = advance_direction(state.topology(), frontier, direction);
            std::vector<SpaceId> continuing;
            for (const auto destination : next) {
                if (decoded->rays && visited[destination.index()]) {
                    continue;
                }
                visited[destination.index()] = true;
                const bool landing_step = decoded->rays || step + 1U == step_limit;
                if (!landing_step) {
                    continuing.push_back(destination);
                    continue;
                }
                const auto landing =
                    inspect_landing(state, occupancy, *actor, destination, *decoded);
                if (landing.candidate) {
                    candidates.push_back(*landing.candidate);
                }
                if (decoded->rays && !landing.blocked) {
                    continuing.push_back(destination);
                }
            }
            frontier = std::move(continuing);
        }
    }

    std::ranges::sort(candidates);
    const auto unique_candidates = std::ranges::unique(candidates).begin();
    candidates.erase(unique_candidates, candidates.end());
    return candidates;
}

} // namespace ludus
