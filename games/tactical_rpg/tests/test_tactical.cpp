#include "ludus/python/runtime.hpp"
#include "ludus/render/snapshot.hpp"
#include "ludus/tactical/game.hpp"
#include "ludus/tactical/presentation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace {

std::unique_ptr<ludus::PythonRuntime> make_runtime() {
    const std::vector<std::string> paths{
        std::string{LUDUS_SOURCE_DIR} + "/python",
        std::string{LUDUS_SOURCE_DIR} + "/games/tactical_rpg/python",
    };
    auto runtime = ludus::PythonRuntime::create(paths);
    INFO((runtime ? std::string{}
                  : runtime.error().message + "\n" + runtime.error().detail));
    REQUIRE(runtime);
    return std::move(*runtime);
}

std::int64_t integer_property(const ludus::GameState& state, ludus::EntityId entity,
                              std::string_view name) {
    const auto property = state.symbols().properties.find(name);
    REQUIRE(property);
    const auto snapshot = state.entities().snapshot(entity);
    REQUIRE(snapshot);
    const auto* value = snapshot->properties.find(*property);
    REQUIRE(value != nullptr);
    const auto* integer = std::get_if<std::int64_t>(value);
    REQUIRE(integer != nullptr);
    return *integer;
}

bool has_tag(const ludus::GameState& state, ludus::EntityId entity,
             std::string_view name) {
    const auto tag = state.symbols().tags.find(name);
    REQUIRE(tag);
    const auto snapshot = state.entities().snapshot(entity);
    REQUIRE(snapshot);
    return snapshot->tags.contains(*tag);
}

ludus::SpaceId location_of(const ludus::GameState& state, ludus::EntityId entity) {
    const auto snapshot = state.entities().snapshot(entity);
    REQUIRE(snapshot);
    REQUIRE(snapshot->location);
    return *snapshot->location;
}

template <typename Payload>
bool has_event(const ludus::EventBatch& batch) {
    return std::ranges::any_of(batch.events, [](const ludus::Event& event) {
        return std::holds_alternative<Payload>(event.payload);
    });
}

void move_to(ludus::tactical::TacticalGame& game, ludus::EntityId actor,
             ludus::tactical::AxialCoord destination) {
    const auto space = game.space(destination);
    REQUIRE(space);
    const auto moved = game.move_unit(actor, *space);
    INFO((moved ? std::string{} : moved.error().message));
    REQUIRE(moved);
}

/// Place the Ranger and Stalker at clear range three, then return to the Ranger's
/// second-round activation using only ordinary recorded actions.
void stage_ranger_duel(ludus::tactical::TacticalGame& game) {
    const auto ranger = game.entity_named("ranger");
    const auto stalker = game.entity_named("stalker");
    const auto guardian = game.entity_named("thorn_guardian");
    const auto hexer = game.entity_named("hexer");
    REQUIRE(ranger);
    REQUIRE(stalker);
    REQUIRE(guardian);
    REQUIRE(hexer);

    move_to(game, *ranger, {-2, 1});
    REQUIRE(game.end_activation());
    move_to(game, *stalker, {1, 1}); // difficult terrain consumes both AP
    REQUIRE(game.end_activation());  // Warden
    REQUIRE(game.end_activation());  // Arcanist
    move_to(game, *guardian, {2, -1});
    REQUIRE(game.end_activation());
    move_to(game, *hexer, {2, 0});
    REQUIRE(game.end_activation());
    REQUIRE(game.active_player() == ludus::tactical::vanguard_player);
}

const ludus::ActionHint& action_to(const ludus::RenderSnapshot& snapshot,
                                   ludus::EntityId actor, ludus::SpaceId destination) {
    const auto found = std::ranges::find_if(snapshot.actions, [&](const ludus::ActionHint& hint) {
        return hint.actor == actor && hint.destination == destination;
    });
    REQUIRE(found != snapshot.actions.end());
    return *found;
}

const ludus::ChoiceHint& choice_named(const ludus::RenderSnapshot& snapshot,
                                      std::string_view prefix) {
    const auto found = std::ranges::find_if(snapshot.choices, [&](const ludus::ChoiceHint& hint) {
        return hint.label.starts_with(prefix);
    });
    REQUIRE(found != snapshot.choices.end());
    return *found;
}

} // namespace

TEST_CASE("scenario version two builds the radius-three shattered shrine",
          "[tactical][topology][scenario-v2]") {
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, 7U);
    REQUIRE(created);
    auto game = std::move(*created);

    REQUIRE(game.scenario_version() == 2U);
    const auto& topology = game.session().state().topology();
    REQUIRE(topology.spaces().size() == 41U);
    REQUIRE(topology.links().size() == 180U);
    REQUIRE(game.space({0, 0}));
    REQUIRE(game.space({3, -3}));
    REQUIRE_FALSE(game.space({4, 0}));

    std::size_t minimum_degree = 6U;
    std::size_t maximum_degree = 0U;
    for (const auto& space : topology.spaces().first(37U)) {
        const auto degree = topology.outgoing(space.id).size();
        minimum_degree = std::min(minimum_degree, degree);
        maximum_degree = std::max(maximum_degree, degree);
    }
    REQUIRE(minimum_degree == 3U);
    REQUIRE(maximum_degree == 6U);

    for (const auto name : {"ranger", "warden", "arcanist", "thorn_guardian",
                            "stalker", "hexer"}) {
        REQUIRE(game.entity_named(name));
    }
    const auto ranger = *game.entity_named("ranger");
    REQUIRE(integer_property(game.session().state(), ranger, "initiative") == 12);
    REQUIRE(integer_property(game.session().state(), *game.entity_named("battle_state"),
                             "action_points") == 2);
}

TEST_CASE("initiative activations enforce AP, one move, and difficult terrain",
          "[tactical][initiative][ap][terrain]") {
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, 19U);
    REQUIRE(created);
    auto game = std::move(*created);
    game.set_hot_seat(true);
    const auto ranger = *game.entity_named("ranger");
    const auto metadata = *game.entity_named("battle_state");

    move_to(game, ranger, {-2, 1});
    REQUIRE(integer_property(game.session().state(), metadata, "action_points") == 1);
    REQUIRE(integer_property(game.session().state(), metadata, "moved") == 1);
    REQUIRE_FALSE(game.move_unit(ranger, *game.space({-1, 1})));
    REQUIRE(game.end_activation());
    REQUIRE(game.active_player() == ludus::tactical::raiders_player);
    const auto stalker = *game.entity_named("stalker");
    move_to(game, stalker, {1, 1});
    REQUIRE(integer_property(game.session().state(), metadata, "action_points") == 2);
    REQUIRE(game.active_player() == ludus::tactical::vanguard_player); // Warden follows
}

TEST_CASE("Ranger choices preserve pause save replay poison and Focus semantics",
          "[tactical][ability][choice][save][replay]") {
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, 0xC0FFEEU);
    REQUIRE(created);
    auto game = std::move(*created);
    game.set_hot_seat(true);
    stage_ranger_duel(game);
    const auto ranger = *game.entity_named("ranger");
    const auto stalker = *game.entity_named("stalker");

    const auto attack = game.begin_attack(ranger, stalker);
    REQUIRE(attack);
    REQUIRE(has_event<ludus::EffectPushed>(*attack));
    REQUIRE(has_event<ludus::ChoiceRequested>(*attack));
    REQUIRE(game.session().state().effect_stack().pending_choice()->options.size() == 3U);

    const auto paused_hash = game.state_hash();
    auto loaded = ludus::GameSession::load(game.session().save());
    REQUIRE(loaded);
    auto restored_result = ludus::tactical::TacticalGame::restore(*runtime, std::move(*loaded));
    REQUIRE(restored_result);
    auto restored = std::move(*restored_result);
    restored.set_hot_seat(true);
    REQUIRE(restored.state_hash() == paused_hash);
    const auto before = integer_property(restored.session().state(), stalker, "health");
    const auto resolved = restored.choose(ludus::tactical::Ability::venom_shot);
    INFO((resolved ? std::string{} : resolved.error().message + " " + resolved.error().detail));
    REQUIRE(resolved);
    REQUIRE(has_event<ludus::ChoiceResolved>(*resolved));
    REQUIRE(has_event<ludus::DiceRolled>(*resolved));
    REQUIRE(integer_property(restored.session().state(), stalker, "health") < before);
    REQUIRE(integer_property(restored.session().state(), stalker, "poison_ticks") == 2);
    REQUIRE(has_tag(restored.session().state(), stalker, "poisoned"));
    REQUIRE(restored.replayed_state_hash() == restored.state_hash());

    const auto poisoned_hash = restored.state_hash();
    REQUIRE(restored.end_activation());
    REQUIRE(integer_property(restored.session().state(), stalker, "poison_ticks") == 1);
    REQUIRE(restored.undo());
    REQUIRE(restored.state_hash() == poisoned_hash);
    REQUIRE(restored.redo());

    auto focused_created = ludus::tactical::TacticalGame::create(*runtime, 77U);
    REQUIRE(focused_created);
    auto focused = std::move(*focused_created);
    focused.set_hot_seat(true);
    stage_ranger_duel(focused);
    const auto focused_ranger = *focused.entity_named("ranger");
    const auto focused_stalker = *focused.entity_named("stalker");
    const auto focus = *focused.entity_named("focus");
    REQUIRE(focused.begin_attack(focused_ranger, focused_stalker));
    REQUIRE(focused.choose(ludus::tactical::Ability::focused_shot));
    REQUIRE(focused.session().state().entities().snapshot(focus)->location == focused.discard());
}

TEST_CASE("Dash and defensive abilities consume AP and expire on the protected activation",
          "[tactical][ability][dash][guard][ward][bulwark]") {
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, 0xDEFEC7U);
    REQUIRE(created);
    auto game = std::move(*created);
    game.set_hot_seat(true);

    const auto stalker = *game.entity_named("stalker");
    const auto warden = *game.entity_named("warden");
    const auto arcanist = *game.entity_named("arcanist");
    const auto guardian = *game.entity_named("thorn_guardian");
    const auto metadata = *game.entity_named("battle_state");

    REQUIRE(game.end_activation()); // Ranger
    const auto dash_destination = *game.space({1, 1});
    REQUIRE(ludus::tactical::hex_distance({3, 0}, {1, 1}) == 2);
    move_to(game, stalker, {1, 1});
    REQUIRE(location_of(game.session().state(), stalker) == dash_destination);
    REQUIRE(game.active_player() == ludus::tactical::vanguard_player);

    REQUIRE(game.use_ability(warden, ludus::tactical::Ability::guard, warden));
    REQUIRE(integer_property(game.session().state(), warden, "armor_bonus") == 2);
    REQUIRE(has_tag(game.session().state(), warden, "guarded"));
    REQUIRE(integer_property(game.session().state(), metadata, "action_points") == 1);
    REQUIRE(game.end_activation());

    REQUIRE(game.use_ability(arcanist, ludus::tactical::Ability::ward, arcanist));
    REQUIRE(integer_property(game.session().state(), arcanist, "armor_bonus") == 2);
    REQUIRE(has_tag(game.session().state(), arcanist, "warded"));
    REQUIRE(game.end_activation());

    REQUIRE(game.use_ability(guardian, ludus::tactical::Ability::bulwark, guardian));
    REQUIRE(integer_property(game.session().state(), guardian, "armor_bonus") == 2);
    REQUIRE(has_tag(game.session().state(), guardian, "bulwark"));
    REQUIRE(game.end_activation());
    REQUIRE(game.end_activation()); // Hexer, wrap to Ranger.
    REQUIRE(game.end_activation()); // Ranger.
    REQUIRE(game.end_activation()); // Stalker, activate Warden.

    REQUIRE(integer_property(game.session().state(), warden, "armor_bonus") == 0);
    REQUIRE_FALSE(has_tag(game.session().state(), warden, "guarded"));
    REQUIRE(integer_property(game.session().state(), arcanist, "armor_bonus") == 2);
    REQUIRE(integer_property(game.session().state(), guardian, "armor_bonus") == 2);
}

TEST_CASE("Arc Bolt ignores armor while Ambush applies its close-range bonus",
          "[tactical][ability][arc-bolt][ambush]") {
    auto runtime = make_runtime();

    SECTION("Arc Bolt") {
        auto created = ludus::tactical::TacticalGame::create(*runtime, 0xA2C0B01U);
        REQUIRE(created);
        auto game = std::move(*created);
        game.set_hot_seat(true);
        const auto arcanist = *game.entity_named("arcanist");
        const auto guardian = *game.entity_named("thorn_guardian");

        REQUIRE(game.end_activation()); // Ranger
        REQUIRE(game.end_activation()); // Stalker
        REQUIRE(game.end_activation()); // Warden
        move_to(game, arcanist, {-1, -1}); // difficult; exhausts AP
        move_to(game, guardian, {2, -1});
        REQUIRE(game.end_activation());
        REQUIRE(game.end_activation()); // Hexer
        REQUIRE(game.end_activation()); // Ranger
        REQUIRE(game.end_activation()); // Stalker
        REQUIRE(game.end_activation()); // Warden

        const auto before = integer_property(game.session().state(), guardian, "health");
        REQUIRE(game.begin_attack(arcanist, guardian));
        REQUIRE(game.choose(ludus::tactical::Ability::arc_bolt));
        const auto damage = before - integer_property(game.session().state(), guardian, "health");
        REQUIRE(damage >= 4); // 1d6 + 3 attack, with the Guardian's armor ignored.
    }

    SECTION("Ambush") {
        auto created = ludus::tactical::TacticalGame::create(*runtime, 0xA4B05U);
        REQUIRE(created);
        auto game = std::move(*created);
        game.set_hot_seat(true);
        const auto ranger = *game.entity_named("ranger");
        const auto stalker = *game.entity_named("stalker");

        move_to(game, ranger, {-2, 1});
        REQUIRE(game.end_activation());
        move_to(game, stalker, {1, 1});
        REQUIRE(game.end_activation()); // Warden
        REQUIRE(game.end_activation()); // Arcanist
        REQUIRE(game.end_activation()); // Guardian
        REQUIRE(game.end_activation()); // Hexer
        REQUIRE(game.end_activation()); // Ranger
        move_to(game, stalker, {0, 1});
        REQUIRE(game.end_activation()); // Warden
        REQUIRE(game.end_activation()); // Arcanist
        REQUIRE(game.end_activation()); // Guardian
        REQUIRE(game.end_activation()); // Hexer
        REQUIRE(game.end_activation()); // Ranger

        const auto before = integer_property(game.session().state(), ranger, "health");
        REQUIRE(game.begin_attack(stalker, ranger));
        REQUIRE(game.choose(ludus::tactical::Ability::ambush));
        REQUIRE(before - integer_property(game.session().state(), ranger, "health") >= 5);
    }
}

TEST_CASE("Crush, Shield Bash, and thorns resolve damage and a clear-hex push",
          "[tactical][ability][crush][shield-bash][reaction]") {
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, 0xBA5EEDU);
    REQUIRE(created);
    auto game = std::move(*created);
    game.set_hot_seat(true);
    const auto warden = *game.entity_named("warden");
    const auto guardian = *game.entity_named("thorn_guardian");

    REQUIRE(game.end_activation()); // Ranger
    REQUIRE(game.end_activation()); // Stalker
    move_to(game, warden, {-2, 0});
    REQUIRE(game.end_activation());
    REQUIRE(game.end_activation()); // Arcanist
    move_to(game, guardian, {2, -1});
    REQUIRE(game.end_activation());
    REQUIRE(game.end_activation()); // Hexer
    REQUIRE(game.end_activation()); // Ranger
    REQUIRE(game.end_activation()); // Stalker
    move_to(game, warden, {-1, -1});
    REQUIRE(game.end_activation()); // Arcanist
    move_to(game, guardian, {1, -1});
    REQUIRE(game.end_activation());
    REQUIRE(game.end_activation()); // Hexer
    REQUIRE(game.end_activation()); // Ranger
    REQUIRE(game.end_activation()); // Stalker
    move_to(game, warden, {0, -1});
    REQUIRE(game.end_activation()); // Arcanist

    const auto warden_before_crush = integer_property(game.session().state(), warden, "health");
    REQUIRE(game.begin_attack(guardian, warden));
    REQUIRE(game.choose(ludus::tactical::Ability::crush));
    REQUIRE(integer_property(game.session().state(), warden, "health") < warden_before_crush);
    REQUIRE(game.end_activation());
    REQUIRE(game.end_activation()); // Hexer
    REQUIRE(game.end_activation()); // Ranger
    REQUIRE(game.end_activation()); // Stalker

    const auto warden_before_thorns = integer_property(game.session().state(), warden, "health");
    const auto guardian_before = integer_property(game.session().state(), guardian, "health");
    REQUIRE(game.begin_attack(warden, guardian));
    REQUIRE(game.choose(ludus::tactical::Ability::shield_bash));
    REQUIRE(integer_property(game.session().state(), guardian, "health") < guardian_before);
    REQUIRE(integer_property(game.session().state(), warden, "health") ==
            warden_before_thorns - 2);
    REQUIRE(location_of(game.session().state(), guardian) == *game.space({2, -1}));
}

TEST_CASE("Blight Bolt poisons and Drain heals exactly the damage dealt",
          "[tactical][ability][blight][drain][poison]") {
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, 0xB11A17U);
    REQUIRE(created);
    auto game = std::move(*created);
    game.set_hot_seat(true);
    const auto ranger = *game.entity_named("ranger");
    const auto hexer = *game.entity_named("hexer");

    move_to(game, ranger, {-2, 1});
    REQUIRE(game.end_activation()); // Ranger
    REQUIRE(game.end_activation()); // Stalker
    REQUIRE(game.end_activation()); // Warden
    REQUIRE(game.end_activation()); // Arcanist
    REQUIRE(game.end_activation()); // Guardian
    move_to(game, hexer, {1, 1});

    const auto hexer_full = integer_property(game.session().state(), hexer, "health");
    REQUIRE(game.begin_attack(ranger, hexer));
    REQUIRE(game.choose(ludus::tactical::Ability::quick_shot));
    REQUIRE(game.end_activation()); // Ranger
    REQUIRE(integer_property(game.session().state(), hexer, "health") < hexer_full);
    REQUIRE(game.end_activation()); // Stalker
    REQUIRE(game.end_activation()); // Warden
    REQUIRE(game.end_activation()); // Arcanist
    REQUIRE(game.end_activation()); // Guardian

    const auto blight_attack = game.begin_attack(hexer, ranger);
    INFO((blight_attack ? std::string{} : blight_attack.error().message + " " +
                                              blight_attack.error().detail));
    REQUIRE(blight_attack);
    REQUIRE(game.choose(ludus::tactical::Ability::blight_bolt));
    REQUIRE(integer_property(game.session().state(), ranger, "poison_ticks") == 2);
    REQUIRE(has_tag(game.session().state(), ranger, "poisoned"));
    REQUIRE(game.end_activation());
    const auto after_poison = integer_property(game.session().state(), ranger, "health");
    REQUIRE(integer_property(game.session().state(), ranger, "poison_ticks") == 1);
    REQUIRE(game.end_activation()); // Ranger
    REQUIRE(game.end_activation()); // Stalker
    REQUIRE(game.end_activation()); // Warden
    REQUIRE(game.end_activation()); // Arcanist
    REQUIRE(game.end_activation()); // Guardian

    const auto hexer_before_drain = integer_property(game.session().state(), hexer, "health");
    const auto ranger_before_drain = integer_property(game.session().state(), ranger, "health");
    REQUIRE(ranger_before_drain == after_poison);
    const auto drain_attack = game.begin_attack(hexer, ranger);
    INFO((drain_attack ? std::string{} : drain_attack.error().message + " " +
                                             drain_attack.error().detail));
    REQUIRE(drain_attack);
    REQUIRE(game.choose(ludus::tactical::Ability::drain));
    const auto damage = ranger_before_drain -
                        integer_property(game.session().state(), ranger, "health");
    REQUIRE(integer_property(game.session().state(), hexer, "health") ==
            std::min<std::int64_t>(hexer_full, hexer_before_drain + damage));
}

TEST_CASE("viewer projections keep fog private while exposing the complete tactical canvas",
          "[tactical][render][fog][hud]") {
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, 31U);
    REQUIRE(created);
    auto game = std::move(*created);
    auto presentation = ludus::tactical::TacticalPresentation::create(game);
    REQUIRE(presentation);
    const auto ranger = *game.entity_named("ranger");
    const auto warden = *game.entity_named("warden");
    const auto stalker = *game.entity_named("stalker");
    const auto own_plan = *game.entity_named("smoke_plan");
    const auto enemy_plan = *game.entity_named("ambush_plan");

    const auto snapshot = presentation->build(game, ludus::tactical::vanguard_player, 1U);
    REQUIRE(snapshot);
    REQUIRE(snapshot->spaces.size() == 41U);
    REQUIRE(snapshot->static_revision == 41U);
    REQUIRE(ludus::find_piece(*snapshot, ranger) != nullptr);
    REQUIRE(ludus::find_piece(*snapshot, warden) != nullptr);
    REQUIRE(ludus::find_piece(*snapshot, stalker) == nullptr);
    REQUIRE(ludus::find_piece(*snapshot, own_plan) != nullptr);
    REQUIRE(ludus::find_piece(*snapshot, enemy_plan) == nullptr);
    REQUIRE(snapshot->bars.size() == 3U);
    const auto board_spaces = std::span{snapshot->spaces}.first(37U);
    REQUIRE(std::ranges::all_of(board_spaces, [](const auto& space) {
        return space.shape == ludus::SpaceShape::hexagon;
    }));
    REQUIRE(snapshot->status.find("2 AP") != std::string::npos);

    const auto view = presentation->build_view(
        game, ludus::tactical::vanguard_player, 2U);
    REQUIRE(view);
    REQUIRE(view->render.revision == 2U);
    REQUIRE(view->units.size() == 3U);
    REQUIRE(view->initiative.size() == 3U);
    REQUIRE(view->objective);
    REQUIRE(view->objective->target == 3);
    REQUIRE_FALSE(view->abilities.empty());
    REQUIRE_FALSE(view->end_state);
}

TEST_CASE("default Raiders AI is deterministic and player undo groups its transactions",
          "[tactical][ai][determinism][undo]") {
    constexpr std::uint64_t seed = 0xB01DFACEU;
    auto runtime = make_runtime();
    auto created = ludus::tactical::TacticalGame::create(*runtime, seed);
    REQUIRE(created);
    auto game = std::move(*created);
    auto presentation = ludus::tactical::TacticalPresentation::create(game);
    REQUIRE(presentation);
    const auto ranger = *game.entity_named("ranger");

    const auto initial = presentation->build(game, ludus::tactical::vanguard_player, 1U);
    REQUIRE(initial);
    const auto destination = *game.space({-2, 1});
    const auto move_token = action_to(*initial, ranger, destination).token;
    const auto first = game.submit_player_token(move_token);
    REQUIRE(first);
    REQUIRE(first->size() == 1U); // Ranger still owns one AP.
    const auto after_move_hash = game.state_hash();

    const auto after_move = presentation->build(game, ludus::tactical::vanguard_player, 2U);
    REQUIRE(after_move);
    const auto end_token = choice_named(*after_move, "End activation").token;
    const auto second = game.submit_player_token(end_token);
    REQUIRE(second);
    REQUIRE(second->size() >= 2U); // Human end plus ordinary recorded AI transactions.
    REQUIRE(game.active_player() == ludus::tactical::vanguard_player);
    const auto completed_hash = game.state_hash();

    REQUIRE(game.undo_player_decision());
    REQUIRE(game.state_hash() == after_move_hash);
    REQUIRE(game.redo_player_decision());
    REQUIRE(game.state_hash() == completed_hash);

    auto mirrored_created = ludus::tactical::TacticalGame::create(*runtime, seed);
    REQUIRE(mirrored_created);
    auto mirrored = std::move(*mirrored_created);
    REQUIRE(mirrored.submit_player_token(move_token));
    REQUIRE(mirrored.submit_player_token(end_token));
    REQUIRE(mirrored.state_hash() == completed_hash);
    REQUIRE(std::vector<std::uint64_t>{mirrored.action_history().begin(),
                                       mirrored.action_history().end()} ==
            std::vector<std::uint64_t>{game.action_history().begin(),
                                       game.action_history().end()});
    REQUIRE_FALSE(game.submit_token(move_token)); // stale token remains rejected.
}
