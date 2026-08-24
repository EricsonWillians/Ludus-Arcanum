#include "ludus/core/binary.hpp"
#include "ludus/core/symbol.hpp"
#include "ludus/core/version.hpp"
#include "ludus/rules/session.hpp"
#include "ludus/topology/topology.hpp"

#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view usage =
    "Usage: ludus-server [--help | --version | --self-check | --demo]";

std::expected<std::string, std::string> deterministic_demo() {
    ludus::SymbolRegistry symbols;
    const auto north = symbols.directions.intern("north");
    const auto east = symbols.directions.intern("east");
    const auto south = symbols.directions.intern("south");
    const auto west = symbols.directions.intern("west");
    const auto spawn_type = symbols.actions.intern("demo.spawn");
    const auto move_type = symbols.actions.intern("demo.move");
    auto topology = ludus::make_rectangular_grid(2U, 2U, {north, east, south, west});
    if (!topology) {
        return std::unexpected(topology.error().message);
    }

    ludus::GameSession session{
        ludus::GameState{std::move(symbols), std::move(*topology)}, 0x4c55445553ULL};
    auto defined = session.define_action(
        ludus::ActionDefinition{spawn_type}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent&) -> std::expected<void, ludus::Diagnostic> {
            auto entity = transaction.spawn(ludus::SpawnOptions{
                .location = ludus::rectangular_space_id(0U, 0U, 2U),
                .owner = ludus::PlayerId{0U, 1U},
                .tags = {},
                .properties = {}});
            if (!entity) {
                return std::unexpected(entity.error());
            }
            return {};
        });
    if (!defined) {
        return std::unexpected(defined.error().message);
    }
    auto spawned = session.submit(
        ludus::ActionIntent{spawn_type, ludus::PlayerId{0U, 1U}, std::nullopt, {}, {}});
    if (!spawned) {
        return std::unexpected(spawned.error().message);
    }
    const auto entity =
        std::get<ludus::EntitySpawned>(spawned->events.front().payload).entity.id;

    defined = session.define_action(
        ludus::ActionDefinition{move_type, 0, true}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& action) {
            return transaction.move(*action.actor, ludus::rectangular_space_id(1U, 1U, 2U));
        });
    if (!defined) {
        return std::unexpected(defined.error().message);
    }
    auto moved = session.submit(
        ludus::ActionIntent{move_type, ludus::PlayerId{0U, 1U}, entity, {}, {}});
    if (!moved) {
        return std::unexpected(moved.error().message);
    }
    const auto replayed = session.replayed_state_hash();
    if (!replayed || *replayed != session.state_hash()) {
        return std::unexpected("deterministic replay hash mismatch");
    }
    auto loaded = ludus::GameSession::load(session.save());
    if (!loaded || loaded->state_hash() != session.state_hash()) {
        return std::unexpected("save/load hash mismatch");
    }

    return "ok: deterministic-demo entities=" +
           std::to_string(session.state().entities().size()) + " events=" +
           std::to_string(spawned->events.size() + moved->events.size()) +
           " hash=" + ludus::hash_hex(session.state_hash());
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 1) {
        std::cout << "Ludus Arcanum headless server " << ludus::version() << '\n';
        return 0;
    }

    const std::string_view argument{argv[1]};
    if (argument == "--help") {
        std::cout << usage << '\n';
        return 0;
    }
    if (argument == "--version") {
        std::cout << ludus::version() << '\n';
        return 0;
    }
    if (argument == "--self-check") {
        std::cout << "ok: ludus-core " << ludus::version() << '\n';
        return 0;
    }
    if (argument == "--demo") {
        const auto result = deterministic_demo();
        if (!result) {
            std::cerr << "demo failed: " << result.error() << '\n';
            return 1;
        }
        std::cout << *result << '\n';
        return 0;
    }

    std::cerr << "Unknown option: " << argument << '\n' << usage << '\n';
    return 2;
}
