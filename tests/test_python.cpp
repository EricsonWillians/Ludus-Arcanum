#include "ludus/python/runtime.hpp"
#include "ludus/rules/session.hpp"
#include "ludus/topology/topology.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct PythonIds {
    ludus::DirectionId forward;
    ludus::PropertyId last_roll;
    ludus::ActionTypeId setup;
    ludus::ActionTypeId python_move;
    ludus::ActionTypeId python_failure;
    ludus::ActionTypeId remember_context;
    ludus::ActionTypeId use_expired_context;
    ludus::ActionTypeId python_roll;
};

struct PythonFixture {
    ludus::GameSession session;
    PythonIds ids;
    ludus::EntityId actor;
};

std::vector<std::string> python_search_paths() {
    return {
        std::string{LUDUS_SOURCE_DIR} + "/python",
        std::string{LUDUS_SOURCE_DIR} + "/tests/python/fixtures",
    };
}

ludus::PythonRuntime& python_runtime() {
    static auto runtime = [] {
        const auto paths = python_search_paths();
        auto created = ludus::PythonRuntime::create(paths);
        if (!created) {
            throw std::runtime_error{created.error().message + "\n" + created.error().detail};
        }
        auto loaded = (*created)->load_module("milestone2_game");
        if (!loaded) {
            throw std::runtime_error{loaded.error().message + "\n" + loaded.error().detail};
        }
        return std::move(*created);
    }();
    return *runtime;
}

TEST_CASE("embedded Python can finalize and recreate its interpreter",
          "[python][embedding][lifecycle]") {
    const auto paths = python_search_paths();
    auto first = ludus::PythonRuntime::create(paths);
    REQUIRE(first);
    REQUIRE((*first)->load_module("milestone2_game"));
    first->reset();

    auto second = ludus::PythonRuntime::create(paths);
    REQUIRE(second);
    REQUIRE((*second)->load_module("milestone2_game"));
}

TEST_CASE("embedded Python rejects malformed package registries during load",
          "[python][embedding][validation]") {
    const auto paths = python_search_paths();
    auto runtime = ludus::PythonRuntime::create(paths);
    REQUIRE(runtime);
    const auto loaded = (*runtime)->load_module("invalid_registry");
    REQUIRE_FALSE(loaded);
    REQUIRE(loaded.error().code == ludus::DiagnosticCode::validation_failed);
    REQUIRE(loaded.error().message.find("callables") != std::string::npos);

    const auto missing = (*runtime)->load_module("ludus_package_that_does_not_exist");
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code == ludus::DiagnosticCode::validation_failed);
    REQUIRE(missing.error().message.find("ModuleNotFoundError") != std::string::npos);
}

PythonFixture make_python_fixture() {
    ludus::SymbolRegistry symbols;
    PythonIds ids;
    ids.forward = symbols.directions.intern("forward");
    ids.last_roll = symbols.properties.intern("last_roll");
    ids.setup = symbols.actions.intern("setup");
    ids.python_move = symbols.actions.intern("python_move");
    ids.python_failure = symbols.actions.intern("python_failure");
    ids.remember_context = symbols.actions.intern("remember_context");
    ids.use_expired_context = symbols.actions.intern("use_expired_context");
    ids.python_roll = symbols.actions.intern("python_roll");

    ludus::TopologyBuilder builder;
    const auto zero = builder.add_space();
    const auto one = builder.add_space();
    const auto two = builder.add_space();
    REQUIRE(builder.add_link(zero, one, ids.forward));
    REQUIRE(builder.add_link(one, two, ids.forward));
    auto topology = std::move(builder).build();
    REQUIRE(topology);

    ludus::GameSession session{ludus::GameState{std::move(symbols), std::move(*topology)}, 99U};
    REQUIRE(session.define_action(
        ludus::ActionDefinition{ids.setup}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent&) -> std::expected<void, ludus::Diagnostic> {
            auto spawned = transaction.spawn(ludus::SpawnOptions{
                .location = ludus::SpaceId{0U, 1U},
                .owner = ludus::PlayerId{0U, 1U},
                .tags = {},
                .properties = {},
            });
            return spawned ? std::expected<void, ludus::Diagnostic>{}
                           : std::unexpected(spawned.error());
        }));
    auto setup = session.submit(
        ludus::ActionIntent{ids.setup, ludus::PlayerId{0U, 1U}, std::nullopt, {}, {}});
    REQUIRE(setup);
    const auto actor =
        std::get<ludus::EntitySpawned>(setup->events.front().payload).entity.id;
    return {std::move(session), ids, actor};
}

void define_python_action(ludus::GameSession& session, ludus::ActionTypeId type,
                          std::string name, bool requires_actor = true) {
    auto* runtime = &python_runtime();
    REQUIRE(session.define_action(
        ludus::ActionDefinition{type, 0, requires_actor}, {},
        [runtime, name = std::move(name)](
            const ludus::RuleContext& context, ludus::Transaction& transaction,
            const ludus::ActionIntent& intent) -> std::expected<void, ludus::Diagnostic> {
            return runtime->invoke_action(name, context.state(), transaction, intent);
        }));
}

ludus::ActionIntent python_intent(ludus::ActionTypeId type, ludus::EntityId actor,
                                  ludus::SpaceId destination) {
    return {type, ludus::PlayerId{0U, 1U}, actor, {destination}, {}};
}

} // namespace

TEST_CASE("embedded Python discovers callbacks and lowers DSL rules to native IR",
          "[python][embedding][rule-ir]") {
    auto fixture = make_python_fixture();
    auto& runtime = python_runtime();
    const auto actions = runtime.action_names();
    const auto movements = runtime.movement_rule_names();

    REQUIRE(actions);
    REQUIRE(actions->size() == 5U);
    REQUIRE(movements);
    REQUIRE(*movements == std::vector<std::string>{"jumper", "slider"});
    const auto program = runtime.compile_movement("slider", fixture.session.state().symbols());
    REQUIRE(program);
    REQUIRE(program->directions().size() == 1U);
    REQUIRE(program->directions().front() == fixture.ids.forward);
    REQUIRE(program->instructions().size() == 5U);

    const auto native = ludus::lower_movement_rule(
        {{fixture.ids.forward},
         {{ludus::RuleOpcode::traverse_rays, 0U},
          {ludus::RuleOpcode::until_blocked, 0U},
          {ludus::RuleOpcode::emit_empty, 0U},
          {ludus::RuleOpcode::emit_enemy_capture, 0U}}});
    REQUIRE(native);
    REQUIRE(*program == *native);

    const auto moves =
        ludus::evaluate_movement(fixture.session.state(), fixture.actor, *program);
    REQUIRE(moves);
    REQUIRE(*moves == std::vector<ludus::MoveCandidate>{
                          {ludus::SpaceId{1U, 1U}, std::nullopt},
                          {ludus::SpaceId{2U, 1U}, std::nullopt}});
}

TEST_CASE("embedded Python has one simulation-thread-confined lifecycle",
          "[python][embedding][lifecycle]") {
    auto& runtime = python_runtime();
    const auto duplicate = ludus::PythonRuntime::create();
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error().code == ludus::DiagnosticCode::invalid_state);

    std::optional<ludus::Diagnostic> worker_error;
    {
        std::jthread worker{[&runtime, &worker_error] {
            const auto actions = runtime.action_names();
            if (!actions) {
                worker_error = actions.error();
            }
        }};
    }
    REQUIRE(worker_error);
    REQUIRE(worker_error->code == ludus::DiagnosticCode::invalid_state);
    REQUIRE(worker_error->message.find("simulation thread") != std::string::npos);
}

TEST_CASE("Python callbacks mutate only through controlled transactions", "[python][transaction]") {
    auto fixture = make_python_fixture();
    define_python_action(fixture.session, fixture.ids.python_move, "python_move");

    const auto moved = fixture.session.submit(
        python_intent(fixture.ids.python_move, fixture.actor, ludus::SpaceId{1U, 1U}));
    REQUIRE(moved);
    REQUIRE(fixture.session.state().entities().snapshot(fixture.actor)->location ==
            ludus::SpaceId{1U, 1U});
    REQUIRE(std::holds_alternative<ludus::EntityMoved>(moved->events.front().payload));
}

TEST_CASE("Python exceptions produce source diagnostics and roll back native state",
          "[python][diagnostics][rollback]") {
    auto fixture = make_python_fixture();
    define_python_action(fixture.session, fixture.ids.python_failure, "python_failure");
    const auto hash_before = fixture.session.state_hash();

    const auto failed = fixture.session.submit(
        python_intent(fixture.ids.python_failure, fixture.actor, ludus::SpaceId{1U, 1U}));
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().message.find("RuntimeError") != std::string::npos);
    REQUIRE(failed.error().message.find("deliberate Python failure") != std::string::npos);
    REQUIRE(failed.error().source.path.find("milestone2_game.py") != std::string::npos);
    REQUIRE(failed.error().source.line > 0U);
    REQUIRE(failed.error().detail.find("Traceback") != std::string::npos);
    REQUIRE(fixture.session.state_hash() == hash_before);
}

TEST_CASE("retained Python contexts and transactions expire after their callback",
          "[python][handles][lifetime]") {
    auto fixture = make_python_fixture();
    define_python_action(fixture.session, fixture.ids.remember_context, "remember_context");
    define_python_action(fixture.session, fixture.ids.use_expired_context,
                         "use_expired_context");
    REQUIRE(fixture.session.submit(python_intent(fixture.ids.remember_context, fixture.actor,
                                                 ludus::SpaceId{1U, 1U})));
    const auto hash_before = fixture.session.state_hash();

    const auto failed = fixture.session.submit(python_intent(
        fixture.ids.use_expired_context, fixture.actor, ludus::SpaceId{2U, 1U}));
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().message.find("capability has expired") != std::string::npos);
    REQUIRE(fixture.session.state_hash() == hash_before);
}

TEST_CASE("Python uses engine-owned deterministic dice and typed properties",
          "[python][random][properties]") {
    auto fixture = make_python_fixture();
    define_python_action(fixture.session, fixture.ids.python_roll, "python_roll");
    const auto rolled = fixture.session.submit(
        python_intent(fixture.ids.python_roll, fixture.actor, ludus::SpaceId{0U, 1U}));

    REQUIRE(rolled);
    REQUIRE(rolled->events.size() == 2U);
    REQUIRE(std::holds_alternative<ludus::DiceRolled>(rolled->events[0].payload));
    const auto snapshot = fixture.session.state().entities().snapshot(fixture.actor);
    REQUIRE(snapshot);
    const auto* value = snapshot->properties.find(fixture.ids.last_roll);
    REQUIRE(value != nullptr);
    if (value == nullptr) {
        return;
    }
    REQUIRE(std::get_if<std::int64_t>(value) != nullptr);
}

TEST_CASE("hot reload is deferred until an explicit safe session boundary",
          "[python][reload]") {
    auto& runtime = python_runtime();
    const auto generation = runtime.generation();
    REQUIRE(runtime.request_reload());

    const auto unsafe = runtime.reload_if_safe(false);
    REQUIRE_FALSE(unsafe);
    REQUIRE(runtime.generation() == generation);
    const auto reloaded = runtime.reload_if_safe(true);
    INFO((reloaded ? std::string{}
                   : reloaded.error().message + "\n" + reloaded.error().detail));
    REQUIRE(reloaded);
    REQUIRE(*reloaded);
    REQUIRE(runtime.generation() == generation + 1U);
}
