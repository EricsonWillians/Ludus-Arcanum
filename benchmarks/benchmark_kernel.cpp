#include "ludus/core/symbol.hpp"
#include "ludus/rule_ir/program.hpp"
#include "ludus/render/animation.hpp"
#include "ludus/render/batch.hpp"
#include "ludus/render/exchange.hpp"
#include "ludus/render/snapshot.hpp"
#include "ludus/rules/random.hpp"
#include "ludus/rules/session.hpp"
#include "ludus/studio/package_document.hpp"
#include "ludus/topology/topology.hpp"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef LUDUS_BENCHMARK_PYTHON
#include "ludus/python/runtime.hpp"
#include "ludus/tactical/game.hpp"
#include "ludus/tactical/presentation.hpp"

#include <string>
#endif

namespace {

struct BenchmarkSession {
    ludus::GameSession session;
    ludus::ActionTypeId move;
    ludus::EntityId entity;
    ludus::DirectionId east;
};

struct StudioPackageFixture {
    StudioPackageFixture()
        : path(std::filesystem::temp_directory_path() /
               ("ludus-studio-benchmark-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
          document(make_document(path)) {}

    ~StudioPackageFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    static ludus::studio::PackageDocument
    make_document(const std::filesystem::path& package_path) {
        auto created = ludus::studio::PackageDocument::create(
            package_path, "org.example.benchmark", 128U, 128U);
        if (!created) {
            throw std::runtime_error{created.error().message};
        }
        return std::move(*created);
    }

    std::filesystem::path path;
    ludus::studio::PackageDocument document;
};

BenchmarkSession make_session(std::size_t move_count) {
    ludus::SymbolRegistry symbols;
    const auto north = symbols.directions.intern("north");
    const auto east = symbols.directions.intern("east");
    const auto south = symbols.directions.intern("south");
    const auto west = symbols.directions.intern("west");
    const auto spawn = symbols.actions.intern("spawn");
    const auto move = symbols.actions.intern("move");
    auto topology = ludus::make_rectangular_grid(8U, 8U, {north, east, south, west});
    if (!topology) {
        throw std::runtime_error{topology.error().message};
    }
    ludus::GameSession session{ludus::GameState{std::move(symbols), std::move(*topology)}, 42U};
    auto defined = session.define_action(
        ludus::ActionDefinition{spawn}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent&) -> std::expected<void, ludus::Diagnostic> {
            auto result = transaction.spawn(
                ludus::SpawnOptions{.location = ludus::rectangular_space_id(0U, 0U, 8U),
                                    .owner = ludus::PlayerId{0U, 1U},
                                    .tags = {},
                                    .properties = {}});
            return result ? std::expected<void, ludus::Diagnostic>{}
                          : std::unexpected(result.error());
        });
    if (!defined) {
        throw std::runtime_error{defined.error().message};
    }
    auto batch = session.submit(
        ludus::ActionIntent{spawn, ludus::PlayerId{0U, 1U}, std::nullopt, {}, {}});
    if (!batch) {
        throw std::runtime_error{batch.error().message};
    }
    const auto entity =
        std::get<ludus::EntitySpawned>(batch->events.front().payload).entity.id;
    defined = session.define_action(
        ludus::ActionDefinition{move, 0, true}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& action) -> std::expected<void, ludus::Diagnostic> {
            return transaction.move(*action.actor, std::get<ludus::SpaceId>(action.targets.front()));
        });
    if (!defined) {
        throw std::runtime_error{defined.error().message};
    }
    for (std::size_t index = 0; index < move_count; ++index) {
        const auto destination = ludus::rectangular_space_id(
            static_cast<std::uint32_t>((index + 1U) % 8U),
            static_cast<std::uint32_t>(((index + 1U) / 8U) % 8U), 8U);
        auto moved = session.submit(ludus::ActionIntent{
            move, ludus::PlayerId{0U, 1U}, entity, {destination}, {}});
        if (!moved) {
            throw std::runtime_error{moved.error().message};
        }
    }
    return BenchmarkSession{std::move(session), move, entity, east};
}

ludus::RenderSnapshot make_render_snapshot() {
    ludus::RenderSnapshot result;
    result.revision = 1U;
    result.world_bounds = {{-4.0F, -4.0F}, {4.0F, 4.0F}};
    result.spaces.reserve(64U);
    for (std::uint32_t square = 0U; square < 64U; ++square) {
        const auto center = ludus::Vec2{static_cast<float>(square % 8U) - 3.5F,
                                        static_cast<float>(square / 8U) - 3.5F};
        result.spaces.push_back(
            {ludus::SpaceId{square, 1U},
             {{center.x - 0.5F, center.y - 0.5F}, {center.x + 0.5F, center.y + 0.5F}},
             {0.3F, 0.4F, 0.5F, 1.0F}});
    }
    result.pieces.reserve(32U);
    for (std::uint32_t index = 0U; index < 32U; ++index) {
        result.pieces.push_back({ludus::EntityId{index, 1U}, ludus::SpaceId{index, 1U},
                                 result.spaces[index].bounds.center(), {0.82F, 0.82F},
                                 ludus::SpriteId{index % 6U}, {}, 1.0F});
    }
    return result;
}

ludus::RenderSnapshot make_render_stress_snapshot() {
    constexpr std::uint32_t extent = 100U;
    ludus::RenderSnapshot result;
    result.revision = 1U;
    result.world_bounds = {{0.0F, 0.0F}, {static_cast<float>(extent),
                                          static_cast<float>(extent)}};
    result.pieces.reserve(static_cast<std::size_t>(extent) * extent);
    for (std::uint32_t index = 0U; index < extent * extent; ++index) {
        result.pieces.push_back(
            {ludus::EntityId{index, 1U}, ludus::SpaceId{index, 1U},
             {static_cast<float>(index % extent) + 0.5F,
              static_cast<float>(index / extent) + 0.5F},
             {0.8F, 0.8F}, ludus::SpriteId{index % 6U}, {}, 1.0F});
    }
    return result;
}

#ifdef LUDUS_BENCHMARK_PYTHON
ludus::PythonRuntime& python_benchmark_runtime() {
    static auto runtime = [] {
        const std::vector<std::string> paths{
            std::string{LUDUS_SOURCE_DIR} + "/python",
            std::string{LUDUS_SOURCE_DIR} + "/benchmarks/python",
            std::string{LUDUS_SOURCE_DIR} + "/games/tactical_rpg/python",
        };
        auto created = ludus::PythonRuntime::create(paths);
        if (!created) {
            throw std::runtime_error{created.error().message};
        }
        const auto loaded = (*created)->load_module("benchmark_rules");
        if (!loaded) {
            throw std::runtime_error{loaded.error().message};
        }
        return std::move(*created);
    }();
    return *runtime;
}

struct NoopSession {
    ludus::GameSession session;
    ludus::ActionTypeId action;
};

NoopSession make_noop_session(bool use_python) {
    ludus::SymbolRegistry symbols;
    const auto action = symbols.actions.intern("noop");
    ludus::TopologyBuilder topology;
    static_cast<void>(topology.add_space());
    auto graph = std::move(topology).build();
    if (!graph) {
        throw std::runtime_error{graph.error().message};
    }
    ludus::GameSession session{ludus::GameState{std::move(symbols), std::move(*graph)}, 42U};
    std::expected<void, ludus::Diagnostic> defined;
    if (use_python) {
        auto* runtime = &python_benchmark_runtime();
        defined = session.define_action(
            ludus::ActionDefinition{action}, {},
            [runtime](const ludus::RuleContext& context, ludus::Transaction& transaction,
                      const ludus::ActionIntent& intent) {
                return runtime->invoke_action("noop", context.state(), transaction, intent);
            });
    } else {
        defined = session.define_action(
            ludus::ActionDefinition{action}, {},
            [](const ludus::RuleContext&, ludus::Transaction&,
               const ludus::ActionIntent&) -> std::expected<void, ludus::Diagnostic> {
                return {};
            });
    }
    if (!defined) {
        throw std::runtime_error{defined.error().message};
    }
    return {std::move(session), action};
}
#endif

void BM_RandomNext(benchmark::State& state) {
    ludus::DeterministicRandom random{42U};
    for (auto _ : state) {
        static_cast<void>(_);
        benchmark::DoNotOptimize(random.next_u32("combat"));
    }
}

void BM_RectangularGrid(benchmark::State& state) {
    ludus::SymbolTable<ludus::DirectionId> symbols;
    const ludus::RectangularDirections directions{
        symbols.intern("north"), symbols.intern("east"), symbols.intern("south"),
        symbols.intern("west")};
    const auto extent = static_cast<std::uint32_t>(state.range(0));
    for (auto _ : state) {
        static_cast<void>(_);
        auto topology = ludus::make_rectangular_grid(extent, extent, directions);
        benchmark::DoNotOptimize(topology);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0) * state.range(0));
}

void BM_TransactionCommitAndUndo(benchmark::State& state) {
    auto fixture = make_session(0U);
    const auto action = ludus::ActionIntent{
        fixture.move, ludus::PlayerId{0U, 1U}, fixture.entity,
        {ludus::rectangular_space_id(1U, 1U, 8U)}, {}};
    for (auto _ : state) {
        static_cast<void>(_);
        auto committed = fixture.session.submit(action);
        benchmark::DoNotOptimize(committed);
        if (!committed || !fixture.session.undo()) {
            state.SkipWithError("transaction benchmark failed");
            break;
        }
    }
}

void BM_Replay128Transactions(benchmark::State& state) {
    const auto fixture = make_session(128U);
    for (auto _ : state) {
        static_cast<void>(_);
        benchmark::DoNotOptimize(fixture.session.replayed_state_hash());
    }
    state.SetItemsProcessed(state.iterations() * 128);
}

void BM_CanonicalStateHash(benchmark::State& state) {
    const auto fixture = make_session(64U);
    for (auto _ : state) {
        static_cast<void>(_);
        benchmark::DoNotOptimize(fixture.session.state_hash());
    }
}

void BM_NativeRayEvaluation(benchmark::State& state) {
    const auto fixture = make_session(0U);
    const auto program = ludus::lower_movement_rule(
        {{fixture.east},
         {{ludus::RuleOpcode::traverse_rays, 0U},
          {ludus::RuleOpcode::until_blocked, 0U},
          {ludus::RuleOpcode::emit_empty, 0U},
          {ludus::RuleOpcode::emit_enemy_capture, 0U}}});
    if (!program) {
        state.SkipWithError(program.error().message);
        return;
    }
    for (auto _ : state) {
        static_cast<void>(_);
        auto moves =
            ludus::evaluate_movement(fixture.session.state(), fixture.entity, *program);
        benchmark::DoNotOptimize(moves);
    }
}

void BM_RenderSnapshotPublish(benchmark::State& state) {
    const auto prototype = make_render_snapshot();
    ludus::RenderSnapshotExchange exchange;
    for (auto _ : state) {
        static_cast<void>(_);
        auto snapshot = prototype;
        benchmark::DoNotOptimize(exchange.publish(std::move(snapshot)));
    }
    state.SetItemsProcessed(state.iterations() * 96);
}

void BM_RenderSnapshotRead(benchmark::State& state) {
    ludus::RenderSnapshotExchange exchange;
    static_cast<void>(exchange.publish(make_render_snapshot()));
    for (auto _ : state) {
        static_cast<void>(_);
        benchmark::DoNotOptimize(exchange.load());
    }
}

void BM_RenderSnapshotPublish10000(benchmark::State& state) {
    const auto prototype = make_render_stress_snapshot();
    ludus::RenderSnapshotExchange exchange;
    for (auto _ : state) {
        static_cast<void>(_);
        auto snapshot = prototype;
        benchmark::DoNotOptimize(exchange.publish(std::move(snapshot)));
    }
    state.SetItemsProcessed(state.iterations() * 10'000);
}

void BM_RenderBatch10000(benchmark::State& state) {
    const auto snapshot = make_render_stress_snapshot();
    ludus::SpriteBatch batch;
    const auto now = std::chrono::steady_clock::time_point{};
    batch.prepare(snapshot, nullptr, std::nullopt, std::nullopt, {}, now);
    for (auto _ : state) {
        static_cast<void>(_);
        batch.prepare(snapshot, nullptr, std::nullopt, std::nullopt, {}, now);
        benchmark::DoNotOptimize(batch.instances().data());
    }
    state.SetItemsProcessed(state.iterations() * 10'000);
}

void BM_RenderPickSpace(benchmark::State& state) {
    const auto snapshot = make_render_snapshot();
    for (auto _ : state) {
        static_cast<void>(_);
        benchmark::DoNotOptimize(ludus::pick_space(snapshot, {0.25F, -1.25F}));
    }
}

void BM_StudioPreview128(benchmark::State& state) {
    StudioPackageFixture fixture;
    std::uint64_t revision = 0U;
    for (auto _ : state) {
        static_cast<void>(_);
        auto snapshot = fixture.document.preview_snapshot(++revision);
        benchmark::DoNotOptimize(snapshot);
        if (!snapshot) {
            state.SkipWithError(snapshot.error().message);
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * 16'384);
}

#ifdef LUDUS_BENCHMARK_PYTHON
void run_noop_action_benchmark(benchmark::State& state, bool use_python) {
    auto fixture = make_noop_session(use_python);
    const ludus::ActionIntent intent{
        fixture.action, ludus::PlayerId{0U, 1U}, std::nullopt, {}, {}};
    for (auto _ : state) {
        static_cast<void>(_);
        auto submitted = fixture.session.submit(intent);
        benchmark::DoNotOptimize(submitted);
        if (!submitted || !fixture.session.undo()) {
            state.SkipWithError("no-op action benchmark failed");
            break;
        }
    }
}

void BM_NativeNoopAction(benchmark::State& state) {
    run_noop_action_benchmark(state, false);
}

void BM_PythonNoopAction(benchmark::State& state) {
    run_noop_action_benchmark(state, true);
}

void BM_TacticalLegalActions(benchmark::State& state) {
    auto created = ludus::tactical::TacticalGame::create(python_benchmark_runtime(), 42U);
    if (!created) {
        state.SkipWithError(created.error().message);
        return;
    }
    auto game = std::move(*created);
    for (auto _ : state) {
        static_cast<void>(_);
        benchmark::DoNotOptimize(
            game.legal_actions(ludus::tactical::vanguard_player));
    }
}

void BM_TacticalViewerSnapshot(benchmark::State& state) {
    auto created = ludus::tactical::TacticalGame::create(python_benchmark_runtime(), 42U);
    if (!created) {
        state.SkipWithError(created.error().message);
        return;
    }
    auto game = std::move(*created);
    auto presentation = ludus::tactical::TacticalPresentation::create(game);
    if (!presentation) {
        state.SkipWithError(presentation.error().message);
        return;
    }
    std::uint64_t revision = 0U;
    for (auto _ : state) {
        static_cast<void>(_);
        benchmark::DoNotOptimize(presentation->build(
            game, ludus::tactical::vanguard_player, ++revision));
    }
}
#endif

} // namespace

BENCHMARK(BM_RandomNext);
BENCHMARK(BM_RectangularGrid)->Arg(8)->Arg(32)->Arg(128);
BENCHMARK(BM_TransactionCommitAndUndo);
BENCHMARK(BM_Replay128Transactions);
BENCHMARK(BM_CanonicalStateHash);
BENCHMARK(BM_NativeRayEvaluation);
BENCHMARK(BM_RenderSnapshotPublish);
BENCHMARK(BM_RenderSnapshotRead);
BENCHMARK(BM_RenderSnapshotPublish10000);
BENCHMARK(BM_RenderBatch10000);
BENCHMARK(BM_RenderPickSpace);
BENCHMARK(BM_StudioPreview128);
#ifdef LUDUS_BENCHMARK_PYTHON
BENCHMARK(BM_NativeNoopAction);
BENCHMARK(BM_PythonNoopAction);
BENCHMARK(BM_TacticalLegalActions);
BENCHMARK(BM_TacticalViewerSnapshot);
#endif
