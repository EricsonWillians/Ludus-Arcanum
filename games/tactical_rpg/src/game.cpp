#include "ludus/tactical/game.hpp"

#include "ludus/core/binary.hpp"
#include "ludus/core/symbol.hpp"
#include "ludus/rules/action.hpp"
#include "ludus/rules/transaction.hpp"
#include "ludus/topology/topology.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ludus::tactical {
namespace {

struct ScenarioLayout {
    std::uint32_t version{2U};
    int radius{3};
    std::uint32_t battlefield_space_count{37U};
    SpaceId vanguard_inventory{37U, 1U};
    SpaceId raiders_inventory{38U, 1U};
    SpaceId deck{39U, 1U};
    SpaceId discard{40U, 1U};
};

constexpr ScenarioLayout scenario_v1{1U, 2, 19U, {19U, 1U}, {20U, 1U},
                                     {21U, 1U}, {22U, 1U}};
constexpr ScenarioLayout scenario_v2{};

struct TacticalIds {
    TagId field;
    TagId container;
    TagId unit;
    TagId card;
    TagId obstacle;
    TagId metadata;
    TagId poisoned;
    TagId thorns;
    TagId guarded;
    TagId warded;
    TagId bulwark;
    TagId cover;
    TagId difficult;
    PropertyId q;
    PropertyId r;
    PropertyId kind;
    PropertyId health;
    PropertyId attack;
    PropertyId armor;
    PropertyId initiative;
    PropertyId phase;
    PropertyId active_player;
    PropertyId round;
    PropertyId next_effect_id;
    PropertyId choice_id;
    PropertyId option_id;
    PropertyId ability;
    PropertyId scenario_version;
    PropertyId active_unit;
    PropertyId action_points;
    PropertyId moved;
    PropertyId offensive_used;
    PropertyId poison_ticks;
    PropertyId armor_bonus;
    PropertyId vanguard_score;
    PropertyId raiders_score;
    PropertyId activation_index;
    PropertyId outcome;
    PropertyId cover_bonus;
    ActionTypeId setup_action;
    ActionTypeId move_action;
    ActionTypeId attack_action;
    ActionTypeId choose_action;
    ActionTypeId end_action;
    ActionTypeId support_action;
    std::array<DirectionId, 6U> directions{};
};

constexpr std::array<AxialCoord, 6U> neighbor_offsets{{
    {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1},
}};

Diagnostic invalid_state(std::string message) {
    return Diagnostic{DiagnosticCode::invalid_state, std::move(message), {}};
}

Diagnostic invalid_action(std::string message) {
    return Diagnostic{DiagnosticCode::validation_failed, std::move(message), {}};
}

void write_id(BinaryWriter& writer, auto id) {
    writer.u32(id.index());
    writer.u32(id.generation());
}

void write_value(BinaryWriter& writer, const PropertyValue& value) {
    writer.u8(static_cast<std::uint8_t>(value.index()));
    std::visit(
        [&writer](const auto& typed) {
            using T = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::same_as<T, bool>) {
                writer.boolean(typed);
            } else if constexpr (std::same_as<T, std::int64_t>) {
                writer.i64(typed);
            } else if constexpr (std::same_as<T, Fixed>) {
                writer.i64(typed.raw());
            } else {
                writer.string(typed);
            }
        },
        value);
}

void set_integer(PropertySet& properties, PropertyId property, std::int64_t value) {
    static_cast<void>(properties.set(property, PropertyValue{value}));
}

void set_string(PropertySet& properties, PropertyId property, std::string value) {
    static_cast<void>(properties.set(property, PropertyValue{std::move(value)}));
}

std::expected<std::int64_t, Diagnostic>
integer_property(const EntitySnapshot& entity, PropertyId property, std::string_view name) {
    const auto* value = entity.properties.find(property);
    const auto* integer = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    if (integer == nullptr) {
        return std::unexpected(invalid_state("tactical entity is missing integer property: " +
                                             std::string{name}));
    }
    return *integer;
}

std::expected<std::string, Diagnostic>
string_property(const EntitySnapshot& entity, PropertyId property, std::string_view name) {
    const auto* value = entity.properties.find(property);
    const auto* string = value == nullptr ? nullptr : std::get_if<std::string>(value);
    if (string == nullptr) {
        return std::unexpected(invalid_state("tactical entity is missing string property: " +
                                             std::string{name}));
    }
    return *string;
}

TacticalIds intern_symbols(SymbolRegistry& symbols) {
    TacticalIds ids;
    ids.field = symbols.tags.intern("tactical_field");
    ids.container = symbols.tags.intern("tactical_container");
    ids.unit = symbols.tags.intern("tactical_unit");
    ids.card = symbols.tags.intern("tactical_card");
    ids.obstacle = symbols.tags.intern("tactical_obstacle");
    ids.metadata = symbols.tags.intern("tactical_metadata");
    ids.poisoned = symbols.tags.intern("poisoned");
    ids.thorns = symbols.tags.intern("thorns");
    ids.guarded = symbols.tags.intern("guarded");
    ids.warded = symbols.tags.intern("warded");
    ids.bulwark = symbols.tags.intern("bulwark");
    ids.cover = symbols.tags.intern("cover");
    ids.difficult = symbols.tags.intern("difficult");
    ids.q = symbols.properties.intern("hex_q");
    ids.r = symbols.properties.intern("hex_r");
    ids.kind = symbols.properties.intern("kind");
    ids.health = symbols.properties.intern("health");
    ids.attack = symbols.properties.intern("attack");
    ids.armor = symbols.properties.intern("armor");
    ids.initiative = symbols.properties.intern("initiative");
    ids.phase = symbols.properties.intern("phase");
    ids.active_player = symbols.properties.intern("active_player");
    ids.round = symbols.properties.intern("round");
    ids.next_effect_id = symbols.properties.intern("next_effect_id");
    ids.choice_id = symbols.properties.intern("choice_id");
    ids.option_id = symbols.properties.intern("option_id");
    ids.ability = symbols.properties.intern("ability");
    ids.scenario_version = symbols.properties.intern("scenario_version");
    ids.active_unit = symbols.properties.intern("active_unit");
    ids.action_points = symbols.properties.intern("action_points");
    ids.moved = symbols.properties.intern("moved");
    ids.offensive_used = symbols.properties.intern("offensive_used");
    ids.poison_ticks = symbols.properties.intern("poison_ticks");
    ids.armor_bonus = symbols.properties.intern("armor_bonus");
    ids.vanguard_score = symbols.properties.intern("vanguard_score");
    ids.raiders_score = symbols.properties.intern("raiders_score");
    ids.activation_index = symbols.properties.intern("activation_index");
    ids.outcome = symbols.properties.intern("outcome");
    ids.cover_bonus = symbols.properties.intern("cover_bonus");
    ids.setup_action = symbols.actions.intern("tactical_setup");
    ids.move_action = symbols.actions.intern("tactical_move");
    ids.attack_action = symbols.actions.intern("tactical_attack");
    ids.choose_action = symbols.actions.intern("tactical_choose_ability");
    ids.end_action = symbols.actions.intern("tactical_end_activation");
    ids.support_action = symbols.actions.intern("tactical_support_ability");
    constexpr std::array<std::string_view, 6U> names{
        "hex_e", "hex_ne", "hex_nw", "hex_w", "hex_sw", "hex_se"};
    for (std::size_t index = 0U; index < names.size(); ++index) {
        ids.directions[index] = symbols.directions.intern(names[index]);
    }
    return ids;
}

template <typename Id>
std::expected<Id, Diagnostic> require_symbol(const SymbolTable<Id>& table,
                                             std::string_view name) {
    auto result = table.find(name);
    if (!result) {
        return std::unexpected(invalid_state(result.error()));
    }
    return *result;
}

std::expected<TacticalIds, Diagnostic> resolve_symbols(const SymbolRegistry& symbols,
                                                       bool version_two) {
    TacticalIds ids;
#define LUDUS_REQUIRE_SYMBOL(member, table, name)                                                \
    do {                                                                                         \
        auto found = require_symbol(symbols.table, name);                                        \
        if (!found) {                                                                            \
            return std::unexpected(found.error());                                               \
        }                                                                                        \
        ids.member = *found;                                                                     \
    } while (false)
    LUDUS_REQUIRE_SYMBOL(field, tags, "tactical_field");
    LUDUS_REQUIRE_SYMBOL(container, tags, "tactical_container");
    LUDUS_REQUIRE_SYMBOL(unit, tags, "tactical_unit");
    LUDUS_REQUIRE_SYMBOL(card, tags, "tactical_card");
    LUDUS_REQUIRE_SYMBOL(obstacle, tags, "tactical_obstacle");
    LUDUS_REQUIRE_SYMBOL(metadata, tags, "tactical_metadata");
    LUDUS_REQUIRE_SYMBOL(poisoned, tags, "poisoned");
    LUDUS_REQUIRE_SYMBOL(thorns, tags, "thorns");
    if (version_two) {
        LUDUS_REQUIRE_SYMBOL(guarded, tags, "guarded");
        LUDUS_REQUIRE_SYMBOL(warded, tags, "warded");
        LUDUS_REQUIRE_SYMBOL(bulwark, tags, "bulwark");
        LUDUS_REQUIRE_SYMBOL(cover, tags, "cover");
        LUDUS_REQUIRE_SYMBOL(difficult, tags, "difficult");
    }
    LUDUS_REQUIRE_SYMBOL(q, properties, "hex_q");
    LUDUS_REQUIRE_SYMBOL(r, properties, "hex_r");
    LUDUS_REQUIRE_SYMBOL(kind, properties, "kind");
    LUDUS_REQUIRE_SYMBOL(health, properties, "health");
    LUDUS_REQUIRE_SYMBOL(attack, properties, "attack");
    LUDUS_REQUIRE_SYMBOL(armor, properties, "armor");
    LUDUS_REQUIRE_SYMBOL(initiative, properties, "initiative");
    LUDUS_REQUIRE_SYMBOL(phase, properties, "phase");
    LUDUS_REQUIRE_SYMBOL(active_player, properties, "active_player");
    LUDUS_REQUIRE_SYMBOL(round, properties, "round");
    LUDUS_REQUIRE_SYMBOL(next_effect_id, properties, "next_effect_id");
    LUDUS_REQUIRE_SYMBOL(choice_id, properties, "choice_id");
    LUDUS_REQUIRE_SYMBOL(option_id, properties, "option_id");
    LUDUS_REQUIRE_SYMBOL(ability, properties, "ability");
    if (version_two) {
        LUDUS_REQUIRE_SYMBOL(scenario_version, properties, "scenario_version");
        LUDUS_REQUIRE_SYMBOL(active_unit, properties, "active_unit");
        LUDUS_REQUIRE_SYMBOL(action_points, properties, "action_points");
        LUDUS_REQUIRE_SYMBOL(moved, properties, "moved");
        LUDUS_REQUIRE_SYMBOL(offensive_used, properties, "offensive_used");
        LUDUS_REQUIRE_SYMBOL(poison_ticks, properties, "poison_ticks");
        LUDUS_REQUIRE_SYMBOL(armor_bonus, properties, "armor_bonus");
        LUDUS_REQUIRE_SYMBOL(vanguard_score, properties, "vanguard_score");
        LUDUS_REQUIRE_SYMBOL(raiders_score, properties, "raiders_score");
        LUDUS_REQUIRE_SYMBOL(activation_index, properties, "activation_index");
        LUDUS_REQUIRE_SYMBOL(outcome, properties, "outcome");
        LUDUS_REQUIRE_SYMBOL(cover_bonus, properties, "cover_bonus");
    }
    LUDUS_REQUIRE_SYMBOL(setup_action, actions, "tactical_setup");
    LUDUS_REQUIRE_SYMBOL(move_action, actions, "tactical_move");
    LUDUS_REQUIRE_SYMBOL(attack_action, actions, "tactical_attack");
    LUDUS_REQUIRE_SYMBOL(choose_action, actions, "tactical_choose_ability");
    if (version_two) {
        LUDUS_REQUIRE_SYMBOL(end_action, actions, "tactical_end_activation");
        LUDUS_REQUIRE_SYMBOL(support_action, actions, "tactical_support_ability");
    }
    constexpr std::array<std::string_view, 6U> direction_names{
        "hex_e", "hex_ne", "hex_nw", "hex_w", "hex_sw", "hex_se"};
    for (std::size_t index = 0U; index < direction_names.size(); ++index) {
        auto found = require_symbol(symbols.directions, direction_names[index]);
        if (!found) {
            return std::unexpected(found.error());
        }
        ids.directions[index] = *found;
    }
#undef LUDUS_REQUIRE_SYMBOL
    return ids;
}

std::vector<AxialCoord> battlefield_coordinates(const ScenarioLayout& layout) {
    std::vector<AxialCoord> result;
    result.reserve(layout.battlefield_space_count);
    for (int q = -layout.radius; q <= layout.radius; ++q) {
        const int minimum_r = std::max(-layout.radius, -q - layout.radius);
        const int maximum_r = std::min(layout.radius, -q + layout.radius);
        for (int r = minimum_r; r <= maximum_r; ++r) {
            result.push_back({q, r});
        }
    }
    return result;
}

std::expected<Topology, Diagnostic>
make_topology(const TacticalIds& ids, const ScenarioLayout& layout,
              std::map<AxialCoord, SpaceId>& spaces) {
    TopologyBuilder builder;
    for (const auto coordinate : battlefield_coordinates(layout)) {
        TagSet tags;
        static_cast<void>(tags.add(ids.field));
        if (layout.version == 2U) {
            constexpr std::array cover_hexes{
                AxialCoord{-2, 0}, AxialCoord{2, 0}, AxialCoord{-1, 2}, AxialCoord{1, -2}};
            constexpr std::array difficult_hexes{
                AxialCoord{-1, -1}, AxialCoord{1, 1}, AxialCoord{0, 1}, AxialCoord{0, -1}};
            if (std::ranges::find(cover_hexes, coordinate) != cover_hexes.end()) {
                static_cast<void>(tags.add(ids.cover));
            }
            if (std::ranges::find(difficult_hexes, coordinate) != difficult_hexes.end()) {
                static_cast<void>(tags.add(ids.difficult));
            }
        }
        PropertySet properties;
        set_integer(properties, ids.q, coordinate.q);
        set_integer(properties, ids.r, coordinate.r);
        const auto space = builder.add_space(std::move(tags), std::move(properties));
        spaces.emplace(coordinate, space);
    }
    TagSet container_tags;
    static_cast<void>(container_tags.add(ids.container));
    for (std::uint32_t index = layout.battlefield_space_count;
         index <= layout.discard.index(); ++index) {
        const auto added = builder.add_space(container_tags);
        if (added != SpaceId{index, 1U}) {
            return std::unexpected(invalid_state("tactical container identity is unstable"));
        }
    }
    for (const auto& [coordinate, from] : spaces) {
        for (std::size_t direction = 0U; direction < neighbor_offsets.size(); ++direction) {
            const AxialCoord neighbor{coordinate.q + neighbor_offsets[direction].q,
                                      coordinate.r + neighbor_offsets[direction].r};
            const auto target = spaces.find(neighbor);
            if (target == spaces.end()) {
                continue;
            }
            if (auto linked = builder.add_link(from, target->second, ids.directions[direction]);
                !linked) {
                return std::unexpected(linked.error());
            }
        }
    }
    return std::move(builder).build();
}

std::expected<std::pair<std::map<AxialCoord, SpaceId>, ScenarioLayout>, Diagnostic>
coordinates_from_topology(const TacticalIds& ids, const Topology& topology) {
    const auto layout = topology.spaces().size() == scenario_v1.discard.index() + 1U
                            ? scenario_v1
                        : topology.spaces().size() == scenario_v2.discard.index() + 1U
                            ? scenario_v2
                            : ScenarioLayout{0U, 0, 0U, {}, {}, {}, {}};
    if (layout.version == 0U) {
        return std::unexpected(invalid_state("tactical topology has an unexpected shape"));
    }
    std::map<AxialCoord, SpaceId> result;
    for (const auto& space : topology.spaces().first(layout.battlefield_space_count)) {
        const auto* q_value = space.properties.find(ids.q);
        const auto* r_value = space.properties.find(ids.r);
        const auto* q = q_value == nullptr ? nullptr : std::get_if<std::int64_t>(q_value);
        const auto* r = r_value == nullptr ? nullptr : std::get_if<std::int64_t>(r_value);
        if (q == nullptr || r == nullptr || *q < std::numeric_limits<int>::min() ||
            *q > std::numeric_limits<int>::max() || *r < std::numeric_limits<int>::min() ||
            *r > std::numeric_limits<int>::max() ||
            !result.emplace(AxialCoord{static_cast<int>(*q), static_cast<int>(*r)}, space.id)
                 .second) {
            return std::unexpected(invalid_state("tactical hex coordinates are malformed"));
        }
    }
    return std::pair{std::move(result), layout};
}

struct StateView {
    EntityId metadata;
    std::int64_t active_player{0};
    std::int64_t round{1};
    std::int64_t next_effect_id{1};
    std::string phase;
    std::int64_t scenario_version{1};
    std::int64_t active_unit{-1};
    std::int64_t action_points{1};
    std::int64_t moved{0};
    std::int64_t offensive_used{0};
    std::int64_t vanguard_score{0};
    std::int64_t raiders_score{0};
    std::int64_t activation_index{0};
    std::string outcome{"ongoing"};
};

std::expected<StateView, Diagnostic> state_view(const TacticalIds& ids,
                                                const GameState& state) {
    std::optional<StateView> result;
    for (const auto entity_id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(entity_id);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        if (!entity->tags.contains(ids.metadata)) {
            continue;
        }
        if (result || entity->location || entity->owner) {
            return std::unexpected(invalid_state("tactical metadata is duplicated or malformed"));
        }
        auto active = integer_property(*entity, ids.active_player, "active_player");
        auto round = integer_property(*entity, ids.round, "round");
        auto next_effect = integer_property(*entity, ids.next_effect_id, "next_effect_id");
        auto phase = string_property(*entity, ids.phase, "phase");
        if (!active || !round || !next_effect || !phase) {
            return std::unexpected(!active   ? active.error()
                                   : !round  ? round.error()
                                   : !next_effect ? next_effect.error()
                                                  : phase.error());
        }
        if ((*active != 0 && *active != 1) || *round < 1 || *next_effect < 1 ||
            (*phase != "act" && *phase != "choose" && *phase != "ended")) {
            return std::unexpected(invalid_state("tactical phase metadata is out of range"));
        }
        result = StateView{entity_id, *active, *round, *next_effect, std::move(*phase)};
        if (ids.scenario_version.valid()) {
            const auto read = [&](PropertyId property, std::string_view name)
                -> std::expected<std::int64_t, Diagnostic> {
                return integer_property(*entity, property, name);
            };
            auto version = read(ids.scenario_version, "scenario_version");
            auto active_unit = read(ids.active_unit, "active_unit");
            auto ap = read(ids.action_points, "action_points");
            auto moved = read(ids.moved, "moved");
            auto offensive = read(ids.offensive_used, "offensive_used");
            auto vanguard_score = read(ids.vanguard_score, "vanguard_score");
            auto raiders_score = read(ids.raiders_score, "raiders_score");
            auto activation = read(ids.activation_index, "activation_index");
            auto outcome = string_property(*entity, ids.outcome, "outcome");
            if (!version || !active_unit || !ap || !moved || !offensive ||
                !vanguard_score || !raiders_score || !activation || !outcome) {
                return std::unexpected(!version ? version.error()
                                       : !active_unit ? active_unit.error()
                                       : !ap ? ap.error()
                                       : !moved ? moved.error()
                                       : !offensive ? offensive.error()
                                       : !vanguard_score ? vanguard_score.error()
                                       : !raiders_score ? raiders_score.error()
                                       : !activation ? activation.error()
                                                     : outcome.error());
            }
            result->scenario_version = *version;
            result->active_unit = *active_unit;
            result->action_points = *ap;
            result->moved = *moved;
            result->offensive_used = *offensive;
            result->vanguard_score = *vanguard_score;
            result->raiders_score = *raiders_score;
            result->activation_index = *activation;
            result->outcome = std::move(*outcome);
            if (*version != 2 || *active_unit < 0 || *ap < 0 || *ap > 2 ||
                (*moved != 0 && *moved != 1) || (*offensive != 0 && *offensive != 1) ||
                *vanguard_score < 0 || *raiders_score < 0 || *activation < 0) {
                return std::unexpected(invalid_state("tactical scenario-v2 metadata is out of range"));
            }
        }
    }
    if (!result) {
        return std::unexpected(invalid_state("tactical metadata is missing"));
    }
    const bool choice_phase = result->phase == "choose";
    if (choice_phase != state.effect_stack().pending_choice().has_value()) {
        return std::unexpected(invalid_state("tactical phase and effect stack disagree"));
    }
    return *result;
}

std::optional<AxialCoord> coordinate_for(const TacticalRuntimeData& runtime, SpaceId space);

bool alive_unit(const TacticalIds& ids, const EntitySnapshot& entity) {
    if (!entity.tags.contains(ids.unit)) {
        return false;
    }
    const auto* value = entity.properties.find(ids.health);
    const auto* health = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    return health != nullptr && *health > 0;
}

std::optional<EntitySnapshot> snapshot_if(const GameState& state, EntityId entity) {
    auto result = state.entities().snapshot(entity);
    return result ? std::optional<EntitySnapshot>{std::move(*result)} : std::nullopt;
}

bool field_is_occupied(const TacticalIds& ids, const GameState& state, SpaceId space) {
    return std::ranges::any_of(state.entities().entities(), [&](EntityId id) {
        const auto entity = state.entities().snapshot(id);
        if (!entity || entity->location != space) {
            return false;
        }
        if (alive_unit(ids, *entity)) {
            return true;
        }
        if (!entity->tags.contains(ids.obstacle)) {
            return false;
        }
        const auto kind = string_property(*entity, ids.kind, "kind");
        return !kind || *kind != "shrine";
    });
}

bool line_of_sight(const TacticalRuntimeData& runtime, const GameState& state,
                   SpaceId from, SpaceId to);

std::optional<EntityId> focus_card(const TacticalRuntimeData& runtime,
                                   const GameState& state, PlayerId owner);

std::expected<void, Diagnostic> spawn_scenario(Transaction& transaction,
                                               const TacticalRuntimeData& runtime);

std::expected<void, Diagnostic> register_actions(GameSession& session,
                                                 std::shared_ptr<TacticalRuntimeData> runtime);

} // namespace

struct TacticalRuntimeData {
    PythonRuntime* python{nullptr};
    TacticalIds ids;
    ScenarioLayout layout;
    std::map<AxialCoord, SpaceId> spaces;
};

namespace {

std::optional<AxialCoord> coordinate_for(const TacticalRuntimeData& runtime, SpaceId space) {
    const auto found = std::ranges::find_if(runtime.spaces, [space](const auto& entry) {
        return entry.second == space;
    });
    return found == runtime.spaces.end() ? std::nullopt
                                         : std::optional<AxialCoord>{found->first};
}

bool line_of_sight(const TacticalRuntimeData& runtime, const GameState& state,
                   SpaceId from, SpaceId to) {
    const auto first = coordinate_for(runtime, from);
    const auto second = coordinate_for(runtime, to);
    if (!first || !second) {
        return false;
    }
    const int distance = hex_distance(*first, *second);
    if (distance == 0 || distance > 3) {
        return false;
    }
    const auto cube_round = [](double x, double y, double z) {
        auto rounded_x = std::round(x);
        auto rounded_y = std::round(y);
        auto rounded_z = std::round(z);
        const auto difference_x = std::abs(rounded_x - x);
        const auto difference_y = std::abs(rounded_y - y);
        const auto difference_z = std::abs(rounded_z - z);
        if (difference_x > difference_y && difference_x > difference_z) {
            rounded_x = -rounded_y - rounded_z;
        } else if (difference_y > difference_z) {
            rounded_y = -rounded_x - rounded_z;
        } else {
            rounded_z = -rounded_x - rounded_y;
        }
        return AxialCoord{static_cast<int>(rounded_x), static_cast<int>(rounded_z)};
    };
    const double first_x = first->q;
    const double first_z = first->r;
    const double first_y = -first_x - first_z;
    const double second_x = second->q;
    const double second_z = second->r;
    const double second_y = -second_x - second_z;
    for (int index = 1; index < distance; ++index) {
        const auto amount = static_cast<double>(index) / static_cast<double>(distance);
        const auto cursor = cube_round(std::lerp(first_x, second_x, amount),
                                       std::lerp(first_y, second_y, amount),
                                       std::lerp(first_z, second_z, amount));
        const auto space = runtime.spaces.find(cursor);
        if (space == runtime.spaces.end() ||
            field_is_occupied(runtime.ids, state, space->second)) {
            return false;
        }
    }
    return true;
}

bool reachable_move(const TacticalRuntimeData& runtime, const GameState& state,
                    SpaceId from, SpaceId to, int maximum_steps) {
    if (from == to || maximum_steps < 1 || to.index() >= runtime.layout.battlefield_space_count ||
        field_is_occupied(runtime.ids, state, to)) {
        return false;
    }
    std::queue<std::pair<SpaceId, int>> open;
    std::map<SpaceId, int> visited;
    open.emplace(from, 0);
    visited.emplace(from, 0);
    while (!open.empty()) {
        const auto [current, distance] = open.front();
        open.pop();
        if (distance >= maximum_steps) {
            continue;
        }
        for (const auto& link : state.topology().outgoing(current)) {
            if (link.to.index() >= runtime.layout.battlefield_space_count ||
                (link.to != to && field_is_occupied(runtime.ids, state, link.to)) ||
                visited.contains(link.to)) {
                continue;
            }
            if (link.to == to) {
                return true;
            }
            visited.emplace(link.to, distance + 1);
            open.emplace(link.to, distance + 1);
        }
    }
    return false;
}

std::optional<EntityId> focus_card(const TacticalRuntimeData& runtime,
                                   const GameState& state, PlayerId owner) {
    const auto expected_space = owner == vanguard_player ? runtime.layout.vanguard_inventory
                                                         : runtime.layout.raiders_inventory;
    for (const auto id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(id);
        if (!entity || !entity->tags.contains(runtime.ids.card) || entity->owner != owner ||
            entity->location != expected_space) {
            continue;
        }
        const auto kind = string_property(*entity, runtime.ids.kind, "kind");
        if (kind && *kind == "focus") {
            return id;
        }
    }
    return std::nullopt;
}

std::expected<void, Diagnostic> spawn_scenario(Transaction& transaction,
                                               const TacticalRuntimeData& runtime) {
    SpawnOptions metadata;
    static_cast<void>(metadata.tags.add(runtime.ids.metadata));
    set_string(metadata.properties, runtime.ids.kind, "battle_state");
    set_string(metadata.properties, runtime.ids.phase, "act");
    set_integer(metadata.properties, runtime.ids.active_player, 0);
    set_integer(metadata.properties, runtime.ids.round, 1);
    set_integer(metadata.properties, runtime.ids.next_effect_id, 1);
    if (runtime.layout.version == 2U) {
        set_integer(metadata.properties, runtime.ids.scenario_version, 2);
        set_integer(metadata.properties, runtime.ids.active_unit, 1);
        set_integer(metadata.properties, runtime.ids.action_points, 2);
        set_integer(metadata.properties, runtime.ids.moved, 0);
        set_integer(metadata.properties, runtime.ids.offensive_used, 0);
        set_integer(metadata.properties, runtime.ids.vanguard_score, 0);
        set_integer(metadata.properties, runtime.ids.raiders_score, 0);
        set_integer(metadata.properties, runtime.ids.activation_index, 0);
        set_string(metadata.properties, runtime.ids.outcome, "ongoing");
    }
    if (auto spawned = transaction.spawn(std::move(metadata)); !spawned) {
        return std::unexpected(spawned.error());
    }

    const auto spawn_unit = [&](std::string kind, PlayerId owner, AxialCoord coordinate,
                                std::int64_t health, std::int64_t attack,
                                std::int64_t armor, std::int64_t initiative,
                                bool thorns) -> std::expected<void, Diagnostic> {
        SpawnOptions unit;
        unit.location = runtime.spaces.at(coordinate);
        unit.owner = owner;
        static_cast<void>(unit.tags.add(runtime.ids.unit));
        if (thorns) {
            static_cast<void>(unit.tags.add(runtime.ids.thorns));
        }
        set_string(unit.properties, runtime.ids.kind, std::move(kind));
        set_integer(unit.properties, runtime.ids.health, health);
        set_integer(unit.properties, runtime.ids.attack, attack);
        set_integer(unit.properties, runtime.ids.armor, armor);
        set_integer(unit.properties, runtime.ids.initiative, initiative);
        if (runtime.layout.version == 2U) {
            set_integer(unit.properties, runtime.ids.poison_ticks, 0);
            set_integer(unit.properties, runtime.ids.armor_bonus, 0);
        }
        auto spawned = transaction.spawn(std::move(unit));
        return spawned ? std::expected<void, Diagnostic>{}
                       : std::expected<void, Diagnostic>{std::unexpected(spawned.error())};
    };
    if (auto spawned = spawn_unit("ranger", vanguard_player, {-3, 1}, 12, 2, 0, 12, false);
        !spawned) {
        return spawned;
    }
    if (auto spawned = spawn_unit("warden", vanguard_player, {-3, 0}, 16, 2, 2, 10, false);
        !spawned) {
        return spawned;
    }
    if (auto spawned = spawn_unit("arcanist", vanguard_player, {-2, -1}, 10, 3, 0, 9,
                                  false);
        !spawned) {
        return spawned;
    }
    if (auto spawned = spawn_unit("thorn_guardian", raiders_player, {3, -1}, 18, 3, 1, 8,
                                  true);
        !spawned) {
        return spawned;
    }
    if (auto spawned = spawn_unit("stalker", raiders_player, {3, 0}, 10, 2, 0, 11, false);
        !spawned) {
        return spawned;
    }
    if (auto spawned = spawn_unit("hexer", raiders_player, {2, 1}, 11, 2, 0, 7, false);
        !spawned) {
        return spawned;
    }

    const auto spawn_obstacle = [&](std::string kind,
                                    AxialCoord coordinate) -> std::expected<void, Diagnostic> {
        SpawnOptions obstacle;
        obstacle.location = runtime.spaces.at(coordinate);
        static_cast<void>(obstacle.tags.add(runtime.ids.obstacle));
        set_string(obstacle.properties, runtime.ids.kind, std::move(kind));
        auto spawned = transaction.spawn(std::move(obstacle));
        return spawned ? std::expected<void, Diagnostic>{}
                       : std::expected<void, Diagnostic>{std::unexpected(spawned.error())};
    };
    for (const auto coordinate : std::array<AxialCoord, 4U>{{{-1, 0}, {1, 0}, {0, -2}, {0, 2}}}) {
        if (auto spawned = spawn_obstacle("ruin", coordinate); !spawned) {
            return spawned;
        }
    }
    if (auto spawned = spawn_obstacle("shrine", {0, 0}); !spawned) {
        return spawned;
    }

    const auto spawn_card = [&](std::string kind, PlayerId owner,
                                SpaceId location) -> std::expected<void, Diagnostic> {
        SpawnOptions card;
        card.location = location;
        card.owner = owner;
        static_cast<void>(card.tags.add(runtime.ids.card));
        set_string(card.properties, runtime.ids.kind, std::move(kind));
        auto spawned = transaction.spawn(std::move(card));
        return spawned ? std::expected<void, Diagnostic>{}
                       : std::expected<void, Diagnostic>{std::unexpected(spawned.error())};
    };
    for (const auto& [kind, owner, location] :
         std::array<std::tuple<std::string, PlayerId, SpaceId>, 4U>{{
             {"focus", vanguard_player, runtime.layout.vanguard_inventory},
             {"focus", raiders_player, runtime.layout.raiders_inventory},
             {"smoke_plan", vanguard_player, runtime.layout.deck},
             {"ambush_plan", raiders_player, runtime.layout.deck},
         }}) {
        if (auto spawned = spawn_card(kind, owner, location); !spawned) {
            return spawned;
        }
    }
    return {};
}

std::vector<EntityId> initiative_order(const TacticalRuntimeData& runtime,
                                       const GameState& state) {
    std::vector<std::pair<std::int64_t, EntityId>> ordered;
    for (const auto id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(id);
        if (!entity || !entity->tags.contains(runtime.ids.unit)) {
            continue;
        }
        const auto initiative = integer_property(*entity, runtime.ids.initiative, "initiative");
        if (initiative) {
            ordered.emplace_back(*initiative, id);
        }
    }
    std::ranges::sort(ordered, [](const auto& left, const auto& right) {
        return left.first == right.first ? left.second < right.second : left.first > right.first;
    });
    std::vector<EntityId> result;
    result.reserve(ordered.size());
    std::ranges::transform(ordered, std::back_inserter(result), &decltype(ordered)::value_type::second);
    return result;
}

std::pair<bool, bool> living_factions(const TacticalRuntimeData& runtime,
                                     const GameState& state) {
    bool vanguard = false;
    bool raiders = false;
    for (const auto id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(id);
        if (!entity || !alive_unit(runtime.ids, *entity)) {
            continue;
        }
        vanguard = vanguard || entity->owner == vanguard_player;
        raiders = raiders || entity->owner == raiders_player;
    }
    return {vanguard, raiders};
}

std::int64_t surviving_health(const TacticalRuntimeData& runtime, const GameState& state,
                              PlayerId player) {
    std::int64_t result = 0;
    for (const auto id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(id);
        if (!entity || entity->owner != player || !entity->tags.contains(runtime.ids.unit)) {
            continue;
        }
        const auto health = integer_property(*entity, runtime.ids.health, "health");
        if (health) {
            result += std::max<std::int64_t>(*health, 0);
        }
    }
    return result;
}

std::expected<void, Diagnostic> finish_game(const TacticalRuntimeData& runtime,
                                            const StateView& view,
                                            std::string outcome,
                                            Transaction& transaction) {
    if (auto changed = transaction.set_property(view.metadata, runtime.ids.outcome,
                                                std::move(outcome)); !changed) {
        return changed;
    }
    if (auto changed = transaction.set_property(view.metadata, runtime.ids.action_points,
                                                std::int64_t{0}); !changed) {
        return changed;
    }
    return transaction.set_property(view.metadata, runtime.ids.phase, std::string{"ended"});
}

std::expected<bool, Diagnostic> resolve_victory(const TacticalRuntimeData& runtime,
                                                const GameState& state,
                                                const StateView& view,
                                                Transaction& transaction) {
    const auto [vanguard_alive, raiders_alive] = living_factions(runtime, state);
    if (!vanguard_alive || !raiders_alive) {
        const auto outcome = vanguard_alive ? "vanguard" : raiders_alive ? "raiders" : "draw";
        if (auto finished = finish_game(runtime, view, outcome, transaction); !finished) {
            return std::unexpected(finished.error());
        }
        return true;
    }
    if (view.vanguard_score >= 3 || view.raiders_score >= 3) {
        const auto outcome = view.vanguard_score >= 3 ? "vanguard" : "raiders";
        if (auto finished = finish_game(runtime, view, outcome, transaction); !finished) {
            return std::unexpected(finished.error());
        }
        return true;
    }
    return false;
}

std::expected<void, Diagnostic> score_round(const TacticalRuntimeData& runtime,
                                            const GameState& state, StateView& view,
                                            Transaction& transaction) {
    const auto shrine = runtime.spaces.find({0, 0});
    bool vanguard = false;
    bool raiders = false;
    if (shrine != runtime.spaces.end()) {
        for (const auto id : state.entities().entities()) {
            const auto entity = state.entities().snapshot(id);
            if (!entity || !alive_unit(runtime.ids, *entity) ||
                entity->location != shrine->second) {
                continue;
            }
            vanguard = vanguard || entity->owner == vanguard_player;
            raiders = raiders || entity->owner == raiders_player;
        }
    }
    if (vanguard != raiders) {
        auto& score = vanguard ? view.vanguard_score : view.raiders_score;
        ++score;
        const auto property = vanguard ? runtime.ids.vanguard_score : runtime.ids.raiders_score;
        if (auto changed = transaction.set_property(view.metadata, property, score); !changed) {
            return changed;
        }
    }
    return {};
}

std::expected<void, Diagnostic> advance_activation(const TacticalRuntimeData& runtime,
                                                   const GameState& state, StateView view,
                                                   Transaction& transaction) {
    const auto order = initiative_order(runtime, state);
    if (order.empty()) {
        return finish_game(runtime, view, "draw", transaction);
    }
    const auto current = std::ranges::find_if(order, [&](EntityId id) {
        return id.index() == static_cast<std::uint32_t>(view.active_unit);
    });
    const auto current_index = current == order.end()
                                   ? 0U
                                   : static_cast<std::size_t>(current - order.begin());
    for (std::size_t offset = 1U; offset <= order.size(); ++offset) {
        const auto next_index = (current_index + offset) % order.size();
        const bool wrapped = current_index + offset >= order.size();
        auto next = state.entities().snapshot(order[next_index]);
        if (!next || !alive_unit(runtime.ids, *next) || !next->owner) {
            continue;
        }
        if (wrapped) {
            if (auto scored = score_round(runtime, state, view, transaction); !scored) {
                return scored;
            }
            ++view.round;
            if (auto changed = transaction.set_property(view.metadata, runtime.ids.round,
                                                        view.round); !changed) {
                return changed;
            }
            auto victory = resolve_victory(runtime, state, view, transaction);
            if (!victory) {
                return std::unexpected(victory.error());
            }
            if (*victory) {
                return {};
            }
            if (view.round > 10) {
                std::string outcome;
                if (view.vanguard_score != view.raiders_score) {
                    outcome = view.vanguard_score > view.raiders_score ? "vanguard" : "raiders";
                } else {
                    const auto first = surviving_health(runtime, state, vanguard_player);
                    const auto second = surviving_health(runtime, state, raiders_player);
                    outcome = first == second ? "draw" : first > second ? "vanguard" : "raiders";
                }
                return finish_game(runtime, view, std::move(outcome), transaction);
            }
        }

        if (auto bonus = integer_property(*next, runtime.ids.armor_bonus, "armor_bonus");
            bonus && *bonus != 0) {
            if (auto changed = transaction.set_property(next->id, runtime.ids.armor_bonus,
                                                        std::int64_t{0}); !changed) {
                return changed;
            }
            for (const auto tag : {runtime.ids.guarded, runtime.ids.warded,
                                   runtime.ids.bulwark}) {
                if (next->tags.contains(tag)) {
                    if (auto removed = transaction.remove_tag(next->id, tag); !removed) {
                        return removed;
                    }
                }
            }
        }
        const auto poison = integer_property(*next, runtime.ids.poison_ticks, "poison_ticks");
        if (poison && *poison > 0) {
            const auto health = integer_property(*next, runtime.ids.health, "health");
            if (!health) {
                return std::unexpected(health.error());
            }
            if (auto changed = transaction.set_property(next->id, runtime.ids.health,
                                                        std::max<std::int64_t>(*health - 1, 0));
                !changed) {
                return changed;
            }
            if (auto changed = transaction.set_property(next->id, runtime.ids.poison_ticks,
                                                        *poison - 1); !changed) {
                return changed;
            }
            if (*poison == 1 && next->tags.contains(runtime.ids.poisoned)) {
                if (auto removed = transaction.remove_tag(next->id, runtime.ids.poisoned);
                    !removed) {
                    return removed;
                }
            }
            if (*health <= 1) {
                continue;
            }
        }
        const auto player_index = next->owner == vanguard_player ? 0 : 1;
        for (const auto& [property, value] :
             std::array<std::pair<PropertyId, std::int64_t>, 6U>{{
                 {runtime.ids.active_unit, static_cast<std::int64_t>(next->id.index())},
                 {runtime.ids.active_player, player_index},
                 {runtime.ids.action_points, 2},
                 {runtime.ids.moved, 0},
                 {runtime.ids.offensive_used, 0},
                 {runtime.ids.activation_index, view.activation_index + 1},
             }}) {
            if (auto changed = transaction.set_property(view.metadata, property, value);
                !changed) {
                return changed;
            }
        }
        return transaction.set_property(view.metadata, runtime.ids.phase, std::string{"act"});
    }
    return finish_game(runtime, view, "draw", transaction);
}

std::expected<void, Diagnostic> advance_turn(const TacticalRuntimeData& runtime,
                                             const GameState& state, const StateView& view,
                                             Transaction& transaction) {
    const auto& ids = runtime.ids;
    if (runtime.layout.version == 2U) {
        return advance_activation(runtime, state, view, transaction);
    }
    const std::int64_t next = view.active_player == 0 ? 1 : 0;
    if (auto changed = transaction.set_property(view.metadata, ids.active_player, next);
        !changed) {
        return changed;
    }
    if (view.active_player == 1) {
        if (auto changed = transaction.set_property(view.metadata, ids.round, view.round + 1);
            !changed) {
            return changed;
        }
    }
    return transaction.set_property(view.metadata, ids.phase, std::string{"act"});
}

std::expected<void, Diagnostic> validate_actor_turn(const TacticalRuntimeData& runtime,
                                                    const GameState& state,
                                                    const ActionIntent& intent,
                                                    EntitySnapshot& actor,
                                                    StateView& view) {
    auto current = state_view(runtime.ids, state);
    if (!current) {
        return std::unexpected(current.error());
    }
    if (!intent.actor || current->phase != "act" ||
        intent.issuer != (current->active_player == 0 ? vanguard_player : raiders_player)) {
        return std::unexpected(invalid_action("actor cannot act in the current phase"));
    }
    auto snapshot = state.entities().snapshot(*intent.actor);
    if (!snapshot || !alive_unit(runtime.ids, *snapshot) || snapshot->owner != intent.issuer ||
        !snapshot->location) {
        return std::unexpected(invalid_action("action actor is not a living active unit"));
    }
    if (runtime.layout.version == 2U &&
        (current->active_unit != static_cast<std::int64_t>(intent.actor->index()) ||
         current->action_points <= 0 || current->outcome != "ongoing")) {
        return std::unexpected(invalid_action("only the active unit may spend action points"));
    }
    actor = std::move(*snapshot);
    view = std::move(*current);
    return {};
}

std::vector<ActionIntent> enumerate_moves(const TacticalRuntimeData& runtime,
                                          const GameState& state, PlayerId player) {
    std::vector<ActionIntent> result;
    const auto view = state_view(runtime.ids, state);
    if (!view || view->phase != "act" ||
        player != (view->active_player == 0 ? vanguard_player : raiders_player)) {
        return result;
    }
    for (const auto id : state.entities().entities()) {
        const auto actor = state.entities().snapshot(id);
        if (!actor || actor->owner != player || !actor->location ||
            !alive_unit(runtime.ids, *actor)) {
            continue;
        }
        if (runtime.layout.version == 2U &&
            (view->active_unit != static_cast<std::int64_t>(id.index()) || view->moved != 0 ||
             view->action_points <= 0)) {
            continue;
        }
        const auto kind = string_property(*actor, runtime.ids.kind, "kind");
        const auto maximum_steps = kind && (*kind == "stalker" || *kind == "scout") ? 2 : 1;
        for (const auto& candidate : state.topology().spaces().first(
                 runtime.layout.battlefield_space_count)) {
            if (!reachable_move(runtime, state, *actor->location, candidate.id,
                                maximum_steps)) {
                continue;
            }
            if (runtime.layout.version == 2U && candidate.tags.contains(runtime.ids.difficult) &&
                view->action_points < 2) {
                continue;
            }
            result.push_back(
                ActionIntent{runtime.ids.move_action, player, id, {candidate.id}, {}});
        }
    }
    return result;
}

std::vector<ActionIntent> enumerate_attacks(const TacticalRuntimeData& runtime,
                                            const GameState& state, PlayerId player) {
    std::vector<ActionIntent> result;
    const auto view = state_view(runtime.ids, state);
    if (!view || view->phase != "act" ||
        player != (view->active_player == 0 ? vanguard_player : raiders_player)) {
        return result;
    }
    for (const auto actor_id : state.entities().entities()) {
        const auto actor = state.entities().snapshot(actor_id);
        if (!actor || actor->owner != player || !actor->location ||
            !alive_unit(runtime.ids, *actor)) {
            continue;
        }
        if (runtime.layout.version == 2U &&
            (view->active_unit != static_cast<std::int64_t>(actor_id.index()) ||
             view->offensive_used != 0 || view->action_points <= 0)) {
            continue;
        }
        for (const auto target_id : state.entities().entities()) {
            const auto target = state.entities().snapshot(target_id);
            if (!target || !target->owner || target->owner == player || !target->location ||
                !alive_unit(runtime.ids, *target)) {
                continue;
            }
            const auto actor_kind = string_property(*actor, runtime.ids.kind, "kind");
            const auto from = coordinate_for(runtime, *actor->location);
            const auto to = coordinate_for(runtime, *target->location);
            const auto maximum_range = actor_kind &&
                                               (*actor_kind == "warden" ||
                                                *actor_kind == "thorn_guardian" ||
                                                *actor_kind == "guardian")
                                           ? 1
                                       : actor_kind &&
                                                 (*actor_kind == "stalker" ||
                                                  *actor_kind == "scout")
                                           ? 2
                                           : 3;
            if (from && to && hex_distance(*from, *to) <= maximum_range &&
                line_of_sight(runtime, state, *actor->location, *target->location)) {
                result.push_back(ActionIntent{runtime.ids.attack_action, player, actor_id,
                                              {target_id}, {}});
            }
        }
    }
    return result;
}

std::expected<void, Diagnostic> register_actions(GameSession& session,
                                                 std::shared_ptr<TacticalRuntimeData> runtime) {
    auto setup = session.define_action(
        ActionDefinition{runtime->ids.setup_action, 0, false},
        [](const RuleContext& context, const ActionIntent&) -> std::expected<void, Diagnostic> {
            if (!context.state().entities().entities().empty()) {
                return std::unexpected(invalid_action("tactical setup is already complete"));
            }
            return {};
        },
        [runtime](const RuleContext&, Transaction& transaction,
                  const ActionIntent&) { return spawn_scenario(transaction, *runtime); });
    if (!setup) {
        return std::unexpected(setup.error());
    }

    auto move = session.define_action(
        ActionDefinition{runtime->ids.move_action, 0, true},
        [runtime](const RuleContext& context,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            EntitySnapshot actor;
            StateView view;
            if (auto valid = validate_actor_turn(*runtime, context.state(), intent, actor, view);
                !valid) {
                return valid;
            }
            if (intent.targets.size() != 1U || !intent.arguments.entries().empty() ||
                !std::holds_alternative<SpaceId>(intent.targets.front())) {
                return std::unexpected(invalid_action("tactical move intent is malformed"));
            }
            if (runtime->layout.version == 2U && view.moved != 0) {
                return std::unexpected(invalid_action("a unit may move only once per activation"));
            }
            const auto destination = std::get<SpaceId>(intent.targets.front());
            const auto kind = string_property(actor, runtime->ids.kind, "kind");
            const auto maximum_steps = kind && (*kind == "stalker" || *kind == "scout") ? 2 : 1;
            if (!reachable_move(*runtime, context.state(), *actor.location, destination,
                                maximum_steps)) {
                return std::unexpected(invalid_action("tactical move destination is blocked"));
            }
            if (runtime->layout.version == 2U) {
                const auto space = context.state().topology().space(destination);
                if (!space) {
                    return std::unexpected(space.error());
                }
                const auto cost = (*space)->tags.contains(runtime->ids.difficult) ? 2 : 1;
                if (view.action_points < cost) {
                    return std::unexpected(invalid_action(
                        "difficult terrain costs two action points"));
                }
            }
            return {};
        },
        [runtime](const RuleContext& context, Transaction& transaction,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            auto view = state_view(runtime->ids, context.state());
            if (!view) {
                return std::unexpected(view.error());
            }
            const auto destination = std::get<SpaceId>(intent.targets.front());
            if (auto moved = transaction.move(*intent.actor, destination); !moved) {
                return moved;
            }
            if (runtime->layout.version == 2U) {
                const auto space = context.state().topology().space(destination);
                if (!space) {
                    return std::unexpected(space.error());
                }
                const auto cost = (*space)->tags.contains(runtime->ids.difficult) ? 2 : 1;
                const auto remaining = view->action_points - cost;
                if (auto changed = transaction.set_property(view->metadata, runtime->ids.moved,
                                                            std::int64_t{1}); !changed) {
                    return changed;
                }
                if (auto changed = transaction.set_property(
                        view->metadata, runtime->ids.action_points, remaining); !changed) {
                    return changed;
                }
                if (remaining > 0 && view->offensive_used == 0) {
                    return {};
                }
            }
            return advance_turn(*runtime, context.state(), *view, transaction);
        },
        [runtime](const RuleContext& context, PlayerId player) {
            return enumerate_moves(*runtime, context.state(), player);
        });
    if (!move) {
        return std::unexpected(move.error());
    }

    if (runtime->layout.version == 2U) {
        auto end = session.define_action(
            ActionDefinition{runtime->ids.end_action, 0, true},
            [runtime](const RuleContext& context,
                      const ActionIntent& intent) -> std::expected<void, Diagnostic> {
                EntitySnapshot actor;
                StateView view;
                if (auto valid =
                        validate_actor_turn(*runtime, context.state(), intent, actor, view);
                    !valid) {
                    return valid;
                }
                if (!intent.targets.empty() || !intent.arguments.entries().empty()) {
                    return std::unexpected(invalid_action("end-activation intent is malformed"));
                }
                return {};
            },
            [runtime](const RuleContext& context, Transaction& transaction,
                      const ActionIntent&) -> std::expected<void, Diagnostic> {
                auto view = state_view(runtime->ids, context.state());
                if (!view) {
                    return std::unexpected(view.error());
                }
                return advance_turn(*runtime, context.state(), *view, transaction);
            },
            [runtime](const RuleContext& context, PlayerId player) {
                std::vector<ActionIntent> result;
                const auto view = state_view(runtime->ids, context.state());
                if (!view || view->phase != "act" || view->outcome != "ongoing" ||
                    player != (view->active_player == 0 ? vanguard_player : raiders_player)) {
                    return result;
                }
                for (const auto id : context.state().entities().entities()) {
                    if (id.index() != static_cast<std::uint32_t>(view->active_unit)) {
                        continue;
                    }
                    result.push_back(ActionIntent{runtime->ids.end_action, player, id, {}, {}});
                    break;
                }
                return result;
            });
        if (!end) {
            return std::unexpected(end.error());
        }

        auto support = session.define_action(
            ActionDefinition{runtime->ids.support_action, 0, true},
            [runtime](const RuleContext& context,
                      const ActionIntent& intent) -> std::expected<void, Diagnostic> {
                EntitySnapshot actor;
                StateView view;
                if (auto valid =
                        validate_actor_turn(*runtime, context.state(), intent, actor, view);
                    !valid) {
                    return valid;
                }
                const auto* value = intent.arguments.find(runtime->ids.ability);
                const auto* integer = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
                if (intent.targets.size() != 1U || intent.arguments.entries().size() != 1U ||
                    !std::holds_alternative<EntityId>(intent.targets.front()) ||
                    integer == nullptr) {
                    return std::unexpected(invalid_action("support ability intent is malformed"));
                }
                const auto ability = static_cast<Ability>(*integer);
                const auto kind = string_property(actor, runtime->ids.kind, "kind");
                const bool matching = kind &&
                    ((ability == Ability::guard && *kind == "warden") ||
                     (ability == Ability::ward && *kind == "arcanist") ||
                     (ability == Ability::bulwark &&
                      (*kind == "thorn_guardian" || *kind == "guardian")));
                const auto target = context.state().entities().snapshot(
                    std::get<EntityId>(intent.targets.front()));
                if (!matching || !target || target->owner != actor.owner ||
                    !target->location || !alive_unit(runtime->ids, *target)) {
                    return std::unexpected(invalid_action("support target or ability is invalid"));
                }
                if (ability == Ability::bulwark && target->id != actor.id) {
                    return std::unexpected(invalid_action("Bulwark targets its user"));
                }
                const auto from = coordinate_for(*runtime, *actor.location);
                const auto to = coordinate_for(*runtime, *target->location);
                const auto range = ability == Ability::ward ? 3 : 1;
                if (!from || !to || hex_distance(*from, *to) > range ||
                    (from != to && !line_of_sight(*runtime, context.state(),
                                                 *actor.location, *target->location))) {
                    return std::unexpected(invalid_action("support target is outside range or line of sight"));
                }
                return {};
            },
            [runtime](const RuleContext& context, Transaction& transaction,
                      const ActionIntent& intent) -> std::expected<void, Diagnostic> {
                auto view = state_view(runtime->ids, context.state());
                if (!view) {
                    return std::unexpected(view.error());
                }
                const auto* value = intent.arguments.find(runtime->ids.ability);
                const auto* integer = value == nullptr
                                          ? nullptr
                                          : std::get_if<std::int64_t>(value);
                if (integer == nullptr || intent.targets.empty() ||
                    !std::holds_alternative<EntityId>(intent.targets.front())) {
                    return std::unexpected(
                        invalid_action("support ability intent became malformed"));
                }
                const auto ability = static_cast<Ability>(*integer);
                const auto target = std::get<EntityId>(intent.targets.front());
                if (auto changed = transaction.set_property(target, runtime->ids.armor_bonus,
                                                            std::int64_t{2}); !changed) {
                    return changed;
                }
                const auto tag = ability == Ability::guard ? runtime->ids.guarded
                                 : ability == Ability::ward ? runtime->ids.warded
                                                            : runtime->ids.bulwark;
                if (auto added = transaction.add_tag(target, tag); !added) {
                    return added;
                }
                const auto remaining = view->action_points - 1;
                if (auto changed = transaction.set_property(view->metadata,
                                                            runtime->ids.action_points,
                                                            remaining); !changed) {
                    return changed;
                }
                return remaining > 0
                           ? transaction.set_property(view->metadata, runtime->ids.phase,
                                                      std::string{"act"})
                           : advance_turn(*runtime, context.state(), *view, transaction);
            },
            [runtime](const RuleContext& context, PlayerId player) {
                std::vector<ActionIntent> result;
                const auto view = state_view(runtime->ids, context.state());
                if (!view || view->phase != "act" || view->action_points < 1 ||
                    player != (view->active_player == 0 ? vanguard_player : raiders_player)) {
                    return result;
                }
                for (const auto actor_id : context.state().entities().entities()) {
                    if (actor_id.index() != static_cast<std::uint32_t>(view->active_unit)) {
                        continue;
                    }
                    const auto actor = context.state().entities().snapshot(actor_id);
                    if (!actor || !actor->location || !actor->owner) {
                        return result;
                    }
                    const auto kind = string_property(*actor, runtime->ids.kind, "kind");
                    const auto ability = kind && *kind == "warden" ? Ability::guard
                                         : kind && *kind == "arcanist" ? Ability::ward
                                         : kind && (*kind == "thorn_guardian" ||
                                                    *kind == "guardian")
                                             ? Ability::bulwark
                                             : static_cast<Ability>(0U);
                    if (static_cast<std::uint32_t>(ability) == 0U) {
                        return result;
                    }
                    for (const auto target_id : context.state().entities().entities()) {
                        const auto target = context.state().entities().snapshot(target_id);
                        if (!target || target->owner != player || !target->location ||
                            !alive_unit(runtime->ids, *target) ||
                            (ability == Ability::bulwark && target_id != actor_id)) {
                            continue;
                        }
                        const auto from = coordinate_for(*runtime, *actor->location);
                        const auto to = coordinate_for(*runtime, *target->location);
                        const auto range = ability == Ability::ward ? 3 : 1;
                        if (!from || !to || hex_distance(*from, *to) > range ||
                            (from != to && !line_of_sight(*runtime, context.state(),
                                                         *actor->location,
                                                         *target->location))) {
                            continue;
                        }
                        ActionIntent intent{runtime->ids.support_action, player, actor_id,
                                            {target_id}, {}};
                        set_integer(intent.arguments, runtime->ids.ability,
                                    static_cast<std::int64_t>(ability));
                        result.push_back(std::move(intent));
                    }
                    break;
                }
                return result;
            });
        if (!support) {
            return std::unexpected(support.error());
        }
    }

    auto attack = session.define_action(
        ActionDefinition{runtime->ids.attack_action, 0, true},
        [runtime](const RuleContext& context,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            EntitySnapshot actor;
            StateView view;
            if (auto valid = validate_actor_turn(*runtime, context.state(), intent, actor, view);
                !valid) {
                return valid;
            }
            if (intent.targets.size() != 1U || !intent.arguments.entries().empty() ||
                !std::holds_alternative<EntityId>(intent.targets.front())) {
                return std::unexpected(invalid_action("tactical attack intent is malformed"));
            }
            if (runtime->layout.version == 2U && view.offensive_used != 0) {
                return std::unexpected(
                    invalid_action("a unit may use only one offensive ability per activation"));
            }
            const auto target = snapshot_if(context.state(),
                                            std::get<EntityId>(intent.targets.front()));
            const auto actor_kind = string_property(actor, runtime->ids.kind, "kind");
            const auto from = coordinate_for(*runtime, *actor.location);
            const auto to = target && target->location
                                ? coordinate_for(*runtime, *target->location)
                                : std::nullopt;
            const auto maximum_range = actor_kind &&
                                               (*actor_kind == "warden" ||
                                                *actor_kind == "thorn_guardian" ||
                                                *actor_kind == "guardian")
                                           ? 1
                                       : actor_kind &&
                                                 (*actor_kind == "stalker" ||
                                                  *actor_kind == "scout")
                                           ? 2
                                           : 3;
            if (!target || !target->owner || target->owner == intent.issuer ||
                !target->location || !alive_unit(runtime->ids, *target) ||
                !from || !to || hex_distance(*from, *to) > maximum_range ||
                !line_of_sight(*runtime, context.state(), *actor.location, *target->location)) {
                return std::unexpected(invalid_action("target is outside range or line of sight"));
            }
            return {};
        },
        [runtime](const RuleContext& context, Transaction& transaction,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            auto view = state_view(runtime->ids, context.state());
            if (!view || view->next_effect_id >= std::numeric_limits<std::int64_t>::max()) {
                return std::unexpected(view ? invalid_state("effect identifier is exhausted")
                                            : view.error());
            }
            const auto effect_id = static_cast<std::uint64_t>(view->next_effect_id);
            if (auto changed = transaction.set_property(view->metadata, runtime->ids.next_effect_id,
                                                        view->next_effect_id + 1);
                !changed) {
                return changed;
            }
            EffectRecord effect;
            effect.id = effect_id;
            effect.continuation = runtime->ids.choose_action;
            effect.source = intent.actor;
            effect.entity_targets.push_back(std::get<EntityId>(intent.targets.front()));
            if (auto pushed = transaction.push_effect(effect); !pushed) {
                return pushed;
            }

            ChoiceWindow choice;
            choice.id = effect_id;
            choice.player = intent.issuer;
            choice.prompt = "Choose an attack ability";
            const auto add_option = [&](Ability ability, std::string label) {
                ChoiceOption option;
                option.id = static_cast<std::uint32_t>(ability);
                option.label = std::move(label);
                set_integer(option.arguments, runtime->ids.ability,
                            static_cast<std::int64_t>(ability));
                choice.options.push_back(std::move(option));
            };
            const auto actor = context.state().entities().snapshot(*intent.actor);
            if (!actor) {
                return std::unexpected(actor.error());
            }
            const auto kind = string_property(*actor, runtime->ids.kind, "kind");
            if (!kind) {
                return std::unexpected(kind.error());
            }
            if (runtime->layout.version == 1U || *kind == "ranger") {
                add_option(Ability::quick_shot, "Quick Shot — 1 AP, 1d6 + attack");
                if (focus_card(*runtime, context.state(), intent.issuer) &&
                    (runtime->layout.version == 1U || view->action_points >= 2)) {
                    add_option(Ability::focused_shot,
                               "Focused Shot — 2 AP, 2d6 + attack, consume Focus");
                }
                add_option(Ability::venom_shot,
                           "Venom Shot — 1 AP, 1d4 + attack, poison 2");
            } else if (*kind == "warden") {
                add_option(Ability::shield_bash,
                           "Shield Bash — 1 AP, melee damage and clear-hex push");
            } else if (*kind == "arcanist") {
                add_option(Ability::arc_bolt,
                           "Arc Bolt — 1 AP, armor-ignoring ranged damage");
            } else if (*kind == "thorn_guardian" || *kind == "guardian") {
                add_option(Ability::crush, "Crush — 1 AP, heavy melee damage");
            } else if (*kind == "stalker" || *kind == "scout") {
                add_option(Ability::ambush, "Ambush — 1 AP, short-range strike");
            } else if (*kind == "hexer") {
                add_option(Ability::blight_bolt,
                           "Blight Bolt — 1 AP, ranged damage and poison 2");
                if (view->action_points >= 2) {
                    add_option(Ability::drain,
                               "Drain — 2 AP, damage and heal damage dealt");
                }
            }
            if (auto requested = transaction.request_choice(std::move(choice)); !requested) {
                return requested;
            }
            return transaction.set_property(view->metadata, runtime->ids.phase,
                                            std::string{"choose"});
        },
        [runtime](const RuleContext& context, PlayerId player) {
            return enumerate_attacks(*runtime, context.state(), player);
        });
    if (!attack) {
        return std::unexpected(attack.error());
    }

    auto choose = session.define_action(
        ActionDefinition{runtime->ids.choose_action, 0, false},
        [runtime](const RuleContext& context,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            const auto& pending = context.state().effect_stack().pending_choice();
            const auto* choice_value = intent.arguments.find(runtime->ids.choice_id);
            const auto* option_value = intent.arguments.find(runtime->ids.option_id);
            const auto* choice_id = choice_value == nullptr
                                        ? nullptr
                                        : std::get_if<std::int64_t>(choice_value);
            const auto* option_id = option_value == nullptr
                                        ? nullptr
                                        : std::get_if<std::int64_t>(option_value);
            const auto* top = context.state().effect_stack().top();
            if (intent.actor || !intent.targets.empty() || intent.arguments.entries().size() != 2U ||
                !pending || intent.issuer != pending->player || choice_id == nullptr ||
                option_id == nullptr || *choice_id <= 0 || *option_id <= 0 ||
                top == nullptr || top->id != pending->id ||
                top->continuation != runtime->ids.choose_action || !top->source ||
                top->entity_targets.size() != 1U ||
                static_cast<std::uint64_t>(*choice_id) != pending->id ||
                std::ranges::none_of(pending->options, [option_id](const ChoiceOption& option) {
                    return option.id == static_cast<std::uint32_t>(*option_id);
                })) {
                return std::unexpected(invalid_action("tactical choice is stale or malformed"));
            }
            return {};
        },
        [runtime](const RuleContext& context, Transaction& transaction,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            auto view = state_view(runtime->ids, context.state());
            const auto* top = context.state().effect_stack().top();
            const auto& pending = context.state().effect_stack().pending_choice();
            if (!view || top == nullptr || !pending) {
                return std::unexpected(view ? invalid_state("pending tactical effect is missing")
                                            : view.error());
            }
            const EffectRecord effect = *top;
            const auto* option_value = intent.arguments.find(runtime->ids.option_id);
            const auto option_id = static_cast<std::uint32_t>(
                *std::get_if<std::int64_t>(option_value));
            auto selected = transaction.resolve_choice(pending->id, option_id);
            if (!selected) {
                return std::unexpected(selected.error());
            }

            ActionIntent callback;
            callback.type = runtime->ids.choose_action;
            callback.issuer = intent.issuer;
            callback.actor = effect.source;
            callback.targets.emplace_back(effect.entity_targets.front());
            callback.arguments = selected->arguments;
            if (runtime->layout.version == 2U) {
                const auto target =
                    context.state().entities().snapshot(effect.entity_targets.front());
                if (target && target->location) {
                    const auto target_space =
                        context.state().topology().space(*target->location);
                    if (target_space && (*target_space)->tags.contains(runtime->ids.cover)) {
                        set_integer(callback.arguments, runtime->ids.cover_bonus, 1);
                    }
                }
            }
            if (option_id == static_cast<std::uint32_t>(Ability::power)) {
                const auto card = focus_card(*runtime, context.state(), intent.issuer);
                if (!card) {
                    return std::unexpected(invalid_state("selected Focus card is missing"));
                }
                callback.targets.emplace_back(*card);
                callback.targets.emplace_back(runtime->layout.discard);
            }
            if (auto invoked = runtime->python->invoke_action(
                    "resolve_attack", context.state(), transaction, callback);
                !invoked) {
                return invoked;
            }
            if (option_id == static_cast<std::uint32_t>(Ability::shield_bash)) {
                const auto actor = context.state().entities().snapshot(*effect.source);
                const auto target =
                    context.state().entities().snapshot(effect.entity_targets.front());
                if (!actor || !target || !actor->location || !target->location) {
                    return std::unexpected(invalid_state("shield-bash participants are missing"));
                }
                const auto from = coordinate_for(*runtime, *actor->location);
                const auto target_coordinate = coordinate_for(*runtime, *target->location);
                if (from && target_coordinate) {
                    const AxialCoord destination_coordinate{
                        target_coordinate->q + (target_coordinate->q - from->q),
                        target_coordinate->r + (target_coordinate->r - from->r)};
                    const auto destination = runtime->spaces.find(destination_coordinate);
                    if (destination != runtime->spaces.end() &&
                        !field_is_occupied(runtime->ids, context.state(), destination->second)) {
                        if (auto moved = transaction.move(target->id, destination->second);
                            !moved) {
                            return moved;
                        }
                    }
                }
            }
            auto popped = transaction.pop_effect(effect.id);
            if (!popped) {
                return std::unexpected(popped.error());
            }
            if (runtime->layout.version == 2U) {
                const auto cost = option_id == static_cast<std::uint32_t>(Ability::focused_shot) ||
                                          option_id == static_cast<std::uint32_t>(Ability::drain)
                                      ? 2
                                      : 1;
                const auto remaining = view->action_points - cost;
                if (remaining < 0) {
                    return std::unexpected(invalid_action("ability costs more action points than remain"));
                }
                if (auto changed = transaction.set_property(
                        view->metadata, runtime->ids.action_points, remaining); !changed) {
                    return changed;
                }
                const auto ability = static_cast<Ability>(option_id);
                const bool offensive = ability != Ability::guard && ability != Ability::ward &&
                                       ability != Ability::bulwark;
                if (offensive) {
                    if (auto changed = transaction.set_property(
                            view->metadata, runtime->ids.offensive_used, std::int64_t{1});
                        !changed) {
                        return changed;
                    }
                }
                if (remaining > 0 && (view->moved == 0 || !offensive)) {
                    return transaction.set_property(view->metadata, runtime->ids.phase,
                                                    std::string{"act"});
                }
            }
            return advance_turn(*runtime, context.state(), *view, transaction);
        },
        [runtime](const RuleContext& context, PlayerId player) {
            std::vector<ActionIntent> result;
            const auto& pending = context.state().effect_stack().pending_choice();
            if (!pending || pending->player != player) {
                return result;
            }
            result.reserve(pending->options.size());
            for (const auto& option : pending->options) {
                ActionIntent intent{runtime->ids.choose_action, player, std::nullopt, {}, {}};
                set_integer(intent.arguments, runtime->ids.choice_id,
                            static_cast<std::int64_t>(pending->id));
                set_integer(intent.arguments, runtime->ids.option_id,
                            static_cast<std::int64_t>(option.id));
                result.push_back(std::move(intent));
            }
            return result;
        });
    if (!choose) {
        return std::unexpected(choose.error());
    }
    return {};
}

std::expected<std::shared_ptr<TacticalRuntimeData>, Diagnostic>
attach_runtime(PythonRuntime& python, const GameState& state, std::string_view rule_module) {
    if (auto loaded = python.load_module(rule_module); !loaded) {
        return std::unexpected(loaded.error());
    }
    const auto names = python.action_names();
    if (!names || std::ranges::find(*names, "resolve_attack") == names->end()) {
        return std::unexpected(names ? invalid_state(
                                           "tactical rule module must register resolve_attack")
                                     : names.error());
    }
    const bool version_two = state.topology().spaces().size() == scenario_v2.discard.index() + 1U;
    auto ids = resolve_symbols(state.symbols(), version_two);
    if (!ids) {
        return std::unexpected(ids.error());
    }
    auto spaces = coordinates_from_topology(*ids, state.topology());
    if (!spaces) {
        return std::unexpected(spaces.error());
    }
    auto runtime = std::make_shared<TacticalRuntimeData>();
    runtime->python = &python;
    runtime->ids = *ids;
    runtime->layout = spaces->second;
    runtime->spaces = std::move(spaces->first);
    return runtime;
}

} // namespace

int hex_distance(AxialCoord first, AxialCoord second) noexcept {
    const int delta_q = first.q - second.q;
    const int delta_r = first.r - second.r;
    return std::max({std::abs(delta_q), std::abs(delta_r), std::abs(delta_q + delta_r)});
}

std::uint64_t encode_action_token(const ActionIntent& intent) {
    BinaryWriter writer;
    writer.u32(intent.type.value());
    write_id(writer, intent.issuer);
    writer.boolean(intent.actor.has_value());
    if (intent.actor) {
        write_id(writer, *intent.actor);
    }
    writer.u64(static_cast<std::uint64_t>(intent.targets.size()));
    for (const auto& target : intent.targets) {
        writer.u8(static_cast<std::uint8_t>(target.index()));
        std::visit([&writer](auto id) { write_id(writer, id); }, target);
    }
    writer.u64(static_cast<std::uint64_t>(intent.arguments.entries().size()));
    for (const auto& argument : intent.arguments.entries()) {
        writer.u32(argument.id.value());
        write_value(writer, argument.value);
    }
    return 0xA000'0000'0000'0000ULL |
           (canonical_hash(writer.data()) & 0x0FFF'FFFF'FFFF'FFFFULL);
}

std::uint64_t encode_choice_token(std::uint64_t choice_id, std::uint32_t option_id) {
    BinaryWriter writer;
    writer.u64(choice_id);
    writer.u32(option_id);
    return 0xC000'0000'0000'0000ULL |
           (canonical_hash(writer.data()) & 0x0FFF'FFFF'FFFF'FFFFULL);
}

std::expected<TacticalGame, Diagnostic>
TacticalGame::create(PythonRuntime& python, std::uint64_t seed, std::string_view rule_module) {
    SymbolRegistry symbols;
    const auto ids = intern_symbols(symbols);
    std::map<AxialCoord, SpaceId> spaces;
    auto topology = make_topology(ids, scenario_v2, spaces);
    if (!topology) {
        return std::unexpected(topology.error());
    }
    if (auto loaded = python.load_module(rule_module); !loaded) {
        return std::unexpected(loaded.error());
    }
    const auto names = python.action_names();
    if (!names || std::ranges::find(*names, "resolve_attack") == names->end()) {
        return std::unexpected(names ? invalid_state(
                                           "tactical rule module must register resolve_attack")
                                     : names.error());
    }
    auto runtime = std::make_shared<TacticalRuntimeData>();
    runtime->python = &python;
    runtime->ids = ids;
    runtime->layout = scenario_v2;
    runtime->spaces = std::move(spaces);
    GameSession session{GameState{std::move(symbols), std::move(*topology)}, seed};
    if (auto registered = register_actions(session, runtime); !registered) {
        return std::unexpected(registered.error());
    }
    const ActionIntent setup{ids.setup_action, vanguard_player, std::nullopt, {}, {}};
    if (auto submitted = session.submit(setup); !submitted) {
        return std::unexpected(submitted.error());
    }
    return TacticalGame{std::move(runtime), std::move(session)};
}

std::expected<TacticalGame, Diagnostic>
TacticalGame::restore(PythonRuntime& python, GameSession session,
                      std::string_view rule_module) {
    auto runtime = attach_runtime(python, session.state(), rule_module);
    if (!runtime) {
        return std::unexpected(runtime.error());
    }
    if (auto checked = state_view((*runtime)->ids, session.state()); !checked) {
        return std::unexpected(checked.error());
    }
    if (auto registered = register_actions(session, *runtime); !registered) {
        return std::unexpected(registered.error());
    }
    return TacticalGame{std::move(*runtime), std::move(session)};
}

std::vector<ActionIntent> TacticalGame::legal_actions(PlayerId player) const {
    return session_.legal_actions(player);
}

void TacticalGame::record_action(std::uint64_t token) {
    if (history_cursor_ < history_.size()) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_cursor_),
                       history_.end());
        history_groups_.erase(
            history_groups_.begin() + static_cast<std::ptrdiff_t>(history_cursor_),
            history_groups_.end());
    }
    const auto group = recording_group_ != 0U ? recording_group_ : ++next_history_group_;
    history_.push_back(token);
    history_groups_.push_back(group);
    history_cursor_ = history_.size();
}

std::expected<EventBatch, Diagnostic> TacticalGame::move_unit(EntityId actor,
                                                               SpaceId destination) {
    const auto snapshot = session_.state().entities().snapshot(actor);
    if (!snapshot || !snapshot->owner) {
        return std::unexpected(invalid_action("move actor has no tactical owner"));
    }
    const ActionIntent intent{runtime_->ids.move_action, *snapshot->owner, actor,
                              {destination}, {}};
    auto submitted = session_.submit(intent);
    if (submitted) {
        record_action(encode_action_token(intent));
    }
    return submitted;
}

std::expected<EventBatch, Diagnostic> TacticalGame::begin_attack(EntityId actor,
                                                                  EntityId target) {
    const auto snapshot = session_.state().entities().snapshot(actor);
    if (!snapshot || !snapshot->owner) {
        return std::unexpected(invalid_action("attack actor has no tactical owner"));
    }
    const ActionIntent intent{runtime_->ids.attack_action, *snapshot->owner, actor,
                              {target}, {}};
    auto submitted = session_.submit(intent);
    if (submitted) {
        record_action(encode_action_token(intent));
    }
    return submitted;
}

std::expected<EventBatch, Diagnostic> TacticalGame::choose(Ability ability) {
    const auto& pending = session_.state().effect_stack().pending_choice();
    if (!pending) {
        return std::unexpected(invalid_action("there is no tactical choice to resolve"));
    }
    const auto choice_id = pending->id;
    ActionIntent intent{runtime_->ids.choose_action, pending->player, std::nullopt, {}, {}};
    set_integer(intent.arguments, runtime_->ids.choice_id,
                static_cast<std::int64_t>(pending->id));
    set_integer(intent.arguments, runtime_->ids.option_id,
                static_cast<std::int64_t>(ability));
    auto submitted = session_.submit(intent);
    if (submitted) {
        record_action(encode_choice_token(choice_id, static_cast<std::uint32_t>(ability)));
    }
    return submitted;
}

std::expected<EventBatch, Diagnostic>
TacticalGame::use_ability(EntityId actor, Ability ability, EntityId target) {
    if (runtime_->layout.version != 2U) {
        return std::unexpected(invalid_action("support abilities require scenario version 2"));
    }
    const auto snapshot = session_.state().entities().snapshot(actor);
    if (!snapshot || !snapshot->owner) {
        return std::unexpected(invalid_action("support actor has no tactical owner"));
    }
    ActionIntent intent{runtime_->ids.support_action, *snapshot->owner, actor, {target}, {}};
    set_integer(intent.arguments, runtime_->ids.ability, static_cast<std::int64_t>(ability));
    auto submitted = session_.submit(intent);
    if (submitted) {
        record_action(encode_action_token(intent));
    }
    return submitted;
}

std::expected<EventBatch, Diagnostic> TacticalGame::end_activation() {
    if (runtime_->layout.version != 2U) {
        return std::unexpected(invalid_action("scenario version 1 has no unit activations"));
    }
    auto player = active_player();
    if (!player) {
        return std::unexpected(player.error());
    }
    const auto actions = legal_actions(*player);
    const auto found = std::ranges::find(actions, runtime_->ids.end_action, &ActionIntent::type);
    if (found == actions.end()) {
        return std::unexpected(invalid_action("there is no activation to end"));
    }
    auto submitted = session_.submit(*found);
    if (submitted) {
        record_action(encode_action_token(*found));
    }
    return submitted;
}

std::expected<EventBatch, Diagnostic> TacticalGame::submit_token(std::uint64_t token) {
    const auto& pending = session_.state().effect_stack().pending_choice();
    if (pending) {
        const ChoiceOption* selected = nullptr;
        for (const auto& option : pending->options) {
            if (encode_choice_token(pending->id, option.id) != token) {
                continue;
            }
            if (selected != nullptr) {
                return std::unexpected(invalid_action("tactical choice token is ambiguous"));
            }
            selected = &option;
        }
        if (selected == nullptr) {
            return std::unexpected(invalid_action("tactical choice token is stale or unknown"));
        }
        return choose(static_cast<Ability>(selected->id));
    }

    auto player = active_player();
    if (!player) {
        return std::unexpected(player.error());
    }
    const auto actions = legal_actions(*player);
    const ActionIntent* selected = nullptr;
    for (const auto& intent : actions) {
        if (encode_action_token(intent) != token) {
            continue;
        }
        if (selected != nullptr) {
            return std::unexpected(invalid_action("tactical action token is ambiguous"));
        }
        selected = &intent;
    }
    if (selected == nullptr) {
        return std::unexpected(invalid_action("tactical action token is stale or unknown"));
    }
    auto submitted = session_.submit(*selected);
    if (submitted) {
        record_action(token);
    }
    return submitted;
}

std::expected<std::vector<EventBatch>, Diagnostic>
TacticalGame::submit_player_token(std::uint64_t token) {
    const auto group = ++next_history_group_;
    recording_group_ = group;
    std::vector<EventBatch> result;
    auto submitted = submit_token(token);
    if (!submitted) {
        recording_group_ = 0U;
        return std::unexpected(submitted.error());
    }
    result.push_back(std::move(*submitted));
    if (ai_enabled_) {
        auto ai_batches = run_ai();
        if (!ai_batches) {
            recording_group_ = 0U;
            return std::unexpected(ai_batches.error());
        }
        std::ranges::move(*ai_batches, std::back_inserter(result));
    }
    recording_group_ = 0U;
    return result;
}

std::expected<std::vector<EventBatch>, Diagnostic> TacticalGame::run_ai() {
    std::vector<EventBatch> result;
    if (!ai_enabled_ || runtime_->layout.version != 2U) {
        return result;
    }
    for (std::size_t transaction = 0U; transaction < 128U; ++transaction) {
        auto player = active_player();
        if (!player) {
            return std::unexpected(player.error());
        }
        if (*player != raiders_player) {
            return result;
        }
        const auto& pending = session_.state().effect_stack().pending_choice();
        if (pending) {
            const ChoiceOption* selected = nullptr;
            int selected_score = std::numeric_limits<int>::min();
            for (const auto& option : pending->options) {
                int score = static_cast<int>(option.id);
                switch (static_cast<Ability>(option.id)) {
                case Ability::drain:
                    score = 900;
                    break;
                case Ability::blight_bolt:
                    score = 800;
                    break;
                case Ability::ambush:
                    score = 700;
                    break;
                case Ability::crush:
                    score = 650;
                    break;
                case Ability::bulwark:
                    score = 200;
                    break;
                default:
                    break;
                }
                if (selected == nullptr || score > selected_score ||
                    (score == selected_score && option.id < selected->id)) {
                    selected = &option;
                    selected_score = score;
                }
            }
            if (selected == nullptr) {
                return std::unexpected(invalid_state("AI received an empty tactical choice"));
            }
            auto committed = choose(static_cast<Ability>(selected->id));
            if (!committed) {
                return std::unexpected(committed.error());
            }
            result.push_back(std::move(*committed));
            continue;
        }

        const auto actions = legal_actions(raiders_player);
        if (actions.empty()) {
            return result;
        }
        const ActionIntent* selected = nullptr;
        std::int64_t selected_score = std::numeric_limits<std::int64_t>::min();
        std::uint64_t selected_token = 0U;
        for (const auto& action : actions) {
            std::int64_t score = -50'000;
            if (!action.targets.empty()) {
                if (const auto* target = std::get_if<EntityId>(&action.targets.front())) {
                    const auto entity = session_.state().entities().snapshot(*target);
                    const auto health = entity
                                            ? integer_property(*entity, runtime_->ids.health,
                                                               "health")
                                            : std::expected<std::int64_t, Diagnostic>{
                                                  std::unexpected(invalid_state("target missing"))};
                    score = 10'000 - (health ? *health * 100 : 0);
                } else if (const auto* destination =
                               std::get_if<SpaceId>(&action.targets.front())) {
                    const auto coordinate = coordinate_for(*runtime_, *destination);
                    if (coordinate) {
                        score = 3'000 - static_cast<std::int64_t>(
                                              hex_distance(*coordinate, {0, 0})) *
                                              250;
                        const auto space = session_.state().topology().space(*destination);
                        if (space && (*space)->tags.contains(runtime_->ids.cover)) {
                            score += 400;
                        }
                        if (*coordinate == AxialCoord{0, 0}) {
                            score += 4'000;
                        }
                    }
                }
            }
            const auto token = encode_action_token(action);
            if (selected == nullptr || score > selected_score ||
                (score == selected_score && token < selected_token)) {
                selected = &action;
                selected_score = score;
                selected_token = token;
            }
        }
        if (selected == nullptr) {
            return result;
        }
        auto committed = session_.submit(*selected);
        if (!committed) {
            return std::unexpected(committed.error());
        }
        record_action(selected_token);
        result.push_back(std::move(*committed));
    }
    return std::unexpected(invalid_state("AI exceeded the bounded 128-transaction turn"));
}

std::expected<void, Diagnostic> TacticalGame::undo() {
    if (history_cursor_ == 0U) {
        return std::unexpected(invalid_state("there is no tactical action to undo"));
    }
    if (auto undone = session_.undo(); !undone) {
        return std::unexpected(undone.error());
    }
    --history_cursor_;
    return {};
}

std::expected<void, Diagnostic> TacticalGame::redo() {
    if (history_cursor_ >= history_.size()) {
        return std::unexpected(invalid_state("there is no tactical action to redo"));
    }
    if (auto redone = session_.redo(); !redone) {
        return std::unexpected(redone.error());
    }
    ++history_cursor_;
    return {};
}

std::expected<void, Diagnostic> TacticalGame::undo_player_decision() {
    if (history_cursor_ == 0U) {
        return std::unexpected(invalid_state("there is no tactical decision to undo"));
    }
    const auto group = history_groups_[history_cursor_ - 1U];
    do {
        if (auto undone = undo(); !undone) {
            return undone;
        }
    } while (history_cursor_ > 0U && history_groups_[history_cursor_ - 1U] == group);
    return {};
}

std::expected<void, Diagnostic> TacticalGame::redo_player_decision() {
    if (history_cursor_ >= history_.size()) {
        return std::unexpected(invalid_state("there is no tactical decision to redo"));
    }
    const auto group = history_groups_[history_cursor_];
    do {
        if (auto redone = redo(); !redone) {
            return redone;
        }
    } while (history_cursor_ < history_.size() && history_groups_[history_cursor_] == group);
    return {};
}

std::expected<PlayerId, Diagnostic> TacticalGame::active_player() const {
    if (const auto& pending = session_.state().effect_stack().pending_choice(); pending) {
        return pending->player;
    }
    auto view = state_view(runtime_->ids, session_.state());
    if (!view) {
        return std::unexpected(view.error());
    }
    return view->active_player == 0 ? vanguard_player : raiders_player;
}

std::optional<SpaceId> TacticalGame::space(AxialCoord coordinate) const {
    const auto found = runtime_->spaces.find(coordinate);
    return found == runtime_->spaces.end() ? std::nullopt
                                           : std::optional<SpaceId>{found->second};
}

std::expected<EntityId, Diagnostic> TacticalGame::entity_named(std::string_view kind) const {
    for (const auto id : session_.state().entities().entities()) {
        const auto entity = session_.state().entities().snapshot(id);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        const auto* value = entity->properties.find(runtime_->ids.kind);
        const auto* name = value == nullptr ? nullptr : std::get_if<std::string>(value);
        if (name != nullptr && *name == kind) {
            return id;
        }
    }
    return std::unexpected(invalid_state("tactical entity is missing: " + std::string{kind}));
}

SpaceId TacticalGame::inventory(PlayerId player) const noexcept {
    return player == vanguard_player ? runtime_->layout.vanguard_inventory
                                     : runtime_->layout.raiders_inventory;
}

SpaceId TacticalGame::discard() const noexcept { return runtime_->layout.discard; }

std::uint32_t TacticalGame::scenario_version() const noexcept {
    return runtime_->layout.version;
}

} // namespace ludus::tactical
