#include "ludus/rule_ir/program.hpp"
#include "ludus/core/binary.hpp"
#include "ludus/rules/session.hpp"
#include "ludus/topology/topology.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace {

struct IrFixture {
    ludus::GameSession session;
    ludus::DirectionId forward;
    ludus::ActionTypeId spawn;
};

IrFixture make_ir_fixture() {
    ludus::SymbolRegistry symbols;
    const auto forward = symbols.directions.intern("forward");
    const auto spawn = symbols.actions.intern("spawn");
    ludus::TopologyBuilder topology;
    const auto zero = topology.add_space();
    const auto one = topology.add_space();
    const auto two = topology.add_space();
    const auto three = topology.add_space();
    REQUIRE(topology.add_link(zero, one, forward));
    REQUIRE(topology.add_link(one, two, forward));
    REQUIRE(topology.add_link(two, three, forward));
    auto graph = std::move(topology).build();
    REQUIRE(graph);

    ludus::GameSession session{ludus::GameState{std::move(symbols), std::move(*graph)}, 13U};
    REQUIRE(session.define_action(
        ludus::ActionDefinition{spawn}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& intent) -> std::expected<void, ludus::Diagnostic> {
            const auto location = std::get<ludus::SpaceId>(intent.targets.front());
            auto entity = transaction.spawn(ludus::SpawnOptions{
                .location = location,
                .owner = intent.issuer,
                .tags = {},
                .properties = {},
            });
            return entity ? std::expected<void, ludus::Diagnostic>{}
                          : std::unexpected(entity.error());
        }));
    return {std::move(session), forward, spawn};
}

ludus::EntityId spawn(IrFixture& fixture, ludus::SpaceId location, ludus::PlayerId owner) {
    auto batch = fixture.session.submit(
        ludus::ActionIntent{fixture.spawn, owner, std::nullopt, {location}, {}});
    REQUIRE(batch);
    return std::get<ludus::EntitySpawned>(batch->events.front().payload).entity.id;
}

ludus::MovementRuleGraph ray_graph(ludus::DirectionId direction) {
    return {{direction},
            {{ludus::RuleOpcode::traverse_rays, 0U},
             {ludus::RuleOpcode::until_blocked, 0U},
             {ludus::RuleOpcode::emit_empty, 0U},
             {ludus::RuleOpcode::emit_enemy_capture, 0U}}};
}

} // namespace

TEST_CASE("movement graphs lower to canonical native bytecode", "[rule-ir][lowering]") {
    const ludus::DirectionId forward{2U};
    auto graph = ray_graph(forward);
    graph.directions.push_back(forward);
    const auto program = ludus::lower_movement_rule(graph);

    REQUIRE(program);
    REQUIRE(program->directions().size() == 1U);
    REQUIRE(program->instructions().size() == 5U);
    REQUIRE(program->instructions().back().opcode == ludus::RuleOpcode::end);

    const auto bytes = program->canonical_bytes();
    const auto restored = ludus::RuleProgram::from_canonical_bytes(bytes);
    REQUIRE(restored);
    REQUIRE(*restored == *program);
    REQUIRE(restored->canonical_hash() == program->canonical_hash());
}

TEST_CASE("equivalent movement graphs lower to identical canonical programs",
          "[rule-ir][lowering][canonical]") {
    const ludus::DirectionId forward{2U};
    auto reordered = ray_graph(forward);
    std::swap(reordered.nodes[2], reordered.nodes[3]);

    const auto canonical = ludus::lower_movement_rule(ray_graph(forward));
    const auto equivalent = ludus::lower_movement_rule(reordered);
    REQUIRE(canonical);
    REQUIRE(equivalent);
    REQUIRE(*canonical == *equivalent);
}

TEST_CASE("serialized rule bytecode rejects invalid direction identifiers",
          "[rule-ir][serialization][validation]") {
    ludus::BinaryWriter writer;
    writer.string("LUDUS-RULE-IR");
    writer.u32(ludus::RuleProgram::bytecode_version);
    writer.u64(1U);
    writer.u32(0U);
    writer.u64(3U);
    writer.u8(static_cast<std::uint8_t>(ludus::RuleOpcode::traverse_jumps));
    writer.u32(1U);
    writer.u8(static_cast<std::uint8_t>(ludus::RuleOpcode::emit_empty));
    writer.u32(0U);
    writer.u8(static_cast<std::uint8_t>(ludus::RuleOpcode::end));
    writer.u32(0U);

    REQUIRE_FALSE(ludus::RuleProgram::from_canonical_bytes(writer.data()));
}

TEST_CASE("native rays emit empty spaces and stop on enemy captures",
          "[rule-ir][rays][occupancy][ownership]") {
    auto fixture = make_ir_fixture();
    const auto actor = spawn(fixture, ludus::SpaceId{0U, 1U}, ludus::PlayerId{0U, 1U});
    const auto enemy = spawn(fixture, ludus::SpaceId{2U, 1U}, ludus::PlayerId{1U, 1U});
    const auto program = ludus::lower_movement_rule(ray_graph(fixture.forward));
    REQUIRE(program);

    const auto moves = ludus::evaluate_movement(fixture.session.state(), actor, *program);
    REQUIRE(moves);
    REQUIRE(*moves == std::vector<ludus::MoveCandidate>{
                          {ludus::SpaceId{1U, 1U}, std::nullopt},
                          {ludus::SpaceId{2U, 1U}, enemy}});
}

TEST_CASE("friendly occupancy blocks native rays without a capture",
          "[rule-ir][rays][blocking]") {
    auto fixture = make_ir_fixture();
    const auto actor = spawn(fixture, ludus::SpaceId{0U, 1U}, ludus::PlayerId{0U, 1U});
    static_cast<void>(
        spawn(fixture, ludus::SpaceId{2U, 1U}, ludus::PlayerId{0U, 1U}));
    const auto program = ludus::lower_movement_rule(ray_graph(fixture.forward));
    REQUIRE(program);

    const auto moves = ludus::evaluate_movement(fixture.session.state(), actor, *program);
    REQUIRE(moves == std::vector<ludus::MoveCandidate>{
                         {ludus::SpaceId{1U, 1U}, std::nullopt}});
}

TEST_CASE("native jumps ignore intermediate occupancy and inspect only the landing space",
          "[rule-ir][jumps]") {
    auto fixture = make_ir_fixture();
    const auto actor = spawn(fixture, ludus::SpaceId{0U, 1U}, ludus::PlayerId{0U, 1U});
    static_cast<void>(
        spawn(fixture, ludus::SpaceId{1U, 1U}, ludus::PlayerId{0U, 1U}));
    const auto program = ludus::lower_movement_rule(
        {{fixture.forward},
         {{ludus::RuleOpcode::traverse_jumps, 2U},
          {ludus::RuleOpcode::emit_empty, 0U},
          {ludus::RuleOpcode::emit_enemy_capture, 0U}}});
    REQUIRE(program);

    const auto moves = ludus::evaluate_movement(fixture.session.state(), actor, *program);
    REQUIRE(moves == std::vector<ludus::MoveCandidate>{
                         {ludus::SpaceId{2U, 1U}, std::nullopt}});
}

TEST_CASE("malformed movement graphs are rejected before evaluation", "[rule-ir][validation]") {
    REQUIRE_FALSE(ludus::lower_movement_rule({{}, {{ludus::RuleOpcode::traverse_rays, 0U}}}));
    REQUIRE_FALSE(ludus::lower_movement_rule(
        {{ludus::DirectionId{1U}},
         {{ludus::RuleOpcode::traverse_jumps, 0U},
          {ludus::RuleOpcode::emit_empty, 0U}}}));
    REQUIRE_FALSE(ludus::lower_movement_rule(
        {{ludus::DirectionId{1U}},
         {{ludus::RuleOpcode::traverse_jumps, 1U},
          {ludus::RuleOpcode::until_blocked, 0U},
          {ludus::RuleOpcode::emit_empty, 0U}}}));
}
