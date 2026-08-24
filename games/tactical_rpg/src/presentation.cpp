#include "ludus/tactical/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ludus::tactical {
namespace {

Diagnostic presentation_error(std::string message) {
    return Diagnostic{DiagnosticCode::invalid_state, std::move(message), {}};
}

template <typename Id>
std::expected<Id, Diagnostic> find_symbol(const SymbolTable<Id>& symbols,
                                          std::string_view name) {
    auto found = symbols.find(name);
    if (!found) {
        return std::unexpected(presentation_error(found.error()));
    }
    return *found;
}

std::optional<std::int64_t> integer(const PropertySet& properties, PropertyId id) {
    const auto* value = properties.find(id);
    const auto* result = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    return result == nullptr ? std::nullopt : std::optional<std::int64_t>{*result};
}

std::optional<std::string_view> string(const PropertySet& properties, PropertyId id) {
    const auto* value = properties.find(id);
    const auto* result = value == nullptr ? nullptr : std::get_if<std::string>(value);
    return result == nullptr ? std::nullopt : std::optional<std::string_view>{*result};
}

Vec2 hex_center(AxialCoord coordinate) {
    constexpr float root_three = 1.7320508F;
    return {root_three * (static_cast<float>(coordinate.q) +
                          static_cast<float>(coordinate.r) * 0.5F),
            1.5F * static_cast<float>(coordinate.r)};
}

Vec2 container_center(std::uint32_t index, std::uint32_t battlefield_space_count) {
    return {7.1F, 2.0F - static_cast<float>(index - battlefield_space_count) * 1.25F};
}

std::optional<AxialCoord> coordinate(const GameState& state, SpaceId space,
                                    PropertyId q_id, PropertyId r_id) {
    const auto topology_space = state.topology().space(space);
    if (!topology_space) {
        return std::nullopt;
    }
    const auto q = integer((*topology_space)->properties, q_id);
    const auto r = integer((*topology_space)->properties, r_id);
    if (!q || !r) {
        return std::nullopt;
    }
    return AxialCoord{static_cast<int>(*q), static_cast<int>(*r)};
}

Vec2 center_for(const GameState& state, SpaceId space, PropertyId q, PropertyId r,
                std::uint32_t battlefield_space_count) {
    const auto hex = coordinate(state, space, q, r);
    return hex ? hex_center(*hex) : container_center(space.index(), battlefield_space_count);
}

bool living(const EntitySnapshot& entity, TagId unit, PropertyId health) {
    const auto value = integer(entity.properties, health);
    return entity.tags.contains(unit) && value && *value > 0;
}

bool unit_is_visible(const GameState& state, const EntitySnapshot& candidate,
                     PlayerId viewer, TagId unit, PropertyId health,
                     PropertyId q, PropertyId r) {
    if (candidate.owner == viewer) {
        return true;
    }
    if (!candidate.location) {
        return false;
    }
    const auto target = coordinate(state, *candidate.location, q, r);
    if (!target) {
        return false;
    }
    for (const auto id : state.entities().entities()) {
        const auto observer = state.entities().snapshot(id);
        if (!observer || observer->owner != viewer || !observer->location ||
            !living(*observer, unit, health)) {
            continue;
        }
        const auto source = coordinate(state, *observer->location, q, r);
        if (source && hex_distance(*source, *target) <= 3) {
            return true;
        }
    }
    return false;
}

bool space_is_visible(const GameState& state, AxialCoord target, PlayerId viewer,
                      TagId unit, PropertyId health, PropertyId q, PropertyId r) {
    for (const auto id : state.entities().entities()) {
        const auto observer = state.entities().snapshot(id);
        if (!observer || observer->owner != viewer || !observer->location ||
            !living(*observer, unit, health)) {
            continue;
        }
        const auto source = coordinate(state, *observer->location, q, r);
        if (source && hex_distance(*source, target) <= 3) {
            return true;
        }
    }
    return false;
}

SpriteId sprite_for(std::string_view kind, bool card, bool obstacle) {
    if (obstacle) {
        return SpriteId{kind == "shrine" ? 6U : 8U};
    }
    if (card) {
        return SpriteId{7U};
    }
    if (kind == "warden") {
        return SpriteId{1U};
    }
    if (kind == "arcanist") {
        return SpriteId{2U};
    }
    if (kind == "guardian" || kind == "thorn_guardian") {
        return SpriteId{3U};
    }
    if (kind == "scout" || kind == "stalker") {
        return SpriteId{4U};
    }
    if (kind == "hexer") {
        return SpriteId{5U};
    }
    return SpriteId{0U};
}

std::int64_t maximum_health(std::string_view kind) {
    if (kind == "ranger") return 12;
    if (kind == "warden") return 16;
    if (kind == "arcanist") return 10;
    if (kind == "guardian" || kind == "thorn_guardian") return 18;
    if (kind == "scout" || kind == "stalker") return 10;
    if (kind == "hexer") return 11;
    return 12;
}

std::string display_name(std::string_view kind) {
    if (kind == "thorn_guardian" || kind == "guardian") return "Thorn Guardian";
    if (kind == "scout" || kind == "stalker") return "Stalker";
    if (kind.empty()) return "Unknown";
    std::string result{kind};
    result.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(result.front())));
    return result;
}

} // namespace

std::expected<TacticalPresentation, Diagnostic>
TacticalPresentation::create(const TacticalGame& game) {
    const auto& symbols = game.session().state().symbols();
    auto unit = find_symbol(symbols.tags, "tactical_unit");
    auto card = find_symbol(symbols.tags, "tactical_card");
    auto obstacle = find_symbol(symbols.tags, "tactical_obstacle");
    auto poisoned = find_symbol(symbols.tags, "poisoned");
    auto kind = find_symbol(symbols.properties, "kind");
    auto health = find_symbol(symbols.properties, "health");
    auto q = find_symbol(symbols.properties, "hex_q");
    auto r = find_symbol(symbols.properties, "hex_r");
    auto active = find_symbol(symbols.properties, "active_player");
    auto round = find_symbol(symbols.properties, "round");
    if (!unit || !card || !obstacle || !poisoned || !kind || !health || !q || !r || !active ||
        !round) {
        return std::unexpected(!unit       ? unit.error()
                               : !card     ? card.error()
                               : !obstacle ? obstacle.error()
                               : !poisoned ? poisoned.error()
                               : !kind     ? kind.error()
                               : !health   ? health.error()
                               : !q        ? q.error()
                               : !r        ? r.error()
                               : !active   ? active.error()
                                           : round.error());
    }
    const auto optional_property = [&](std::string_view name) -> std::optional<PropertyId> {
        const auto found = symbols.properties.find(name);
        return found ? std::optional<PropertyId>{*found} : std::nullopt;
    };
    return TacticalPresentation{
        *unit, *card, *obstacle, *poisoned, *kind, *health, *q, *r, *active, *round,
        optional_property("active_unit"), optional_property("action_points"),
        optional_property("vanguard_score"), optional_property("raiders_score"),
        optional_property("outcome")};
}

std::expected<RenderSnapshot, Diagnostic>
TacticalPresentation::build(const TacticalGame& game, PlayerId viewer,
                            std::uint64_t revision) const {
    if (viewer != vanguard_player && viewer != raiders_player) {
        return std::unexpected(presentation_error("tactical viewer is invalid"));
    }
    const auto& state = game.session().state();
    RenderSnapshot result;
    result.revision = revision;
    result.static_revision = state.topology().spaces().size();
    result.dynamic_revision = revision;
    result.world_bounds = {{-6.2F, -5.1F}, {8.0F, 5.1F}};
    const auto battlefield_space_count = static_cast<std::uint32_t>(std::ranges::count_if(
        state.topology().spaces(), [&](const Space& space) {
            return integer(space.properties, q_).has_value() &&
                   integer(space.properties, r_).has_value();
        }));
    result.spaces.reserve(state.topology().spaces().size());
    for (const auto& space : state.topology().spaces()) {
        const auto center = center_for(state, space.id, q_, r_, battlefield_space_count);
        const auto axial = coordinate(state, space.id, q_, r_);
        const bool battlefield = axial.has_value();
        const bool shrine = axial == AxialCoord{0, 0};
        const bool visible = !axial || space_is_visible(state, *axial, viewer, unit_, health_,
                                                        q_, r_);
        const bool cover = axial == AxialCoord{-2, 0} || axial == AxialCoord{2, 0} ||
                           axial == AxialCoord{-1, 2} || axial == AxialCoord{1, -2};
        const bool difficult = axial == AxialCoord{-1, -1} || axial == AxialCoord{1, 1} ||
                               axial == AxialCoord{0, 1} || axial == AxialCoord{0, -1};
        Color color = shrine
                                ? Color{0.26F, 0.16F, 0.37F, 1.0F}
                            : cover ? Color{0.16F, 0.22F, 0.18F, 1.0F}
                            : difficult ? Color{0.19F, 0.135F, 0.12F, 1.0F}
                            : battlefield && space.id.index() % 2U == 0U
                                ? Color{0.105F, 0.145F, 0.17F, 1.0F}
                            : battlefield ? Color{0.135F, 0.18F, 0.19F, 1.0F}
                                          : Color{0.19F, 0.15F, 0.24F, 0.96F};
        if (!visible) {
            color = {color.red * 0.28F, color.green * 0.3F, color.blue * 0.36F, 1.0F};
        }
        const Vec2 half = battlefield ? Vec2{0.965F, 0.93F} : Vec2{0.68F, 0.50F};
        result.spaces.push_back(SpaceVisual{
            space.id,
            {{center.x - half.x, center.y - half.y}, {center.x + half.x, center.y + half.y}},
            color,
            battlefield ? SpaceShape::hexagon : SpaceShape::rounded_rectangle,
            shrine ? Color{0.66F, 0.32F, 0.95F, 0.92F}
                   : Color{0.38F, 0.45F, 0.49F, battlefield ? 0.72F : 0.45F},
            shrine ? 0.08F : 0.035F});
    }
    for (const auto& link : state.topology().links()) {
        if (link.from < link.to && coordinate(state, link.from, q_, r_) &&
            coordinate(state, link.to, q_, r_)) {
            result.links.push_back(LinkVisual{link.from, link.to,
                                              center_for(state, link.from, q_, r_, battlefield_space_count),
                                              center_for(state, link.to, q_, r_, battlefield_space_count),
                                              Color{0.25F, 0.31F, 0.33F, 0.28F}, 0.018F});
        }
    }

    for (const auto id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(id);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        if (!entity->location) {
            continue;
        }
        const bool is_unit = entity->tags.contains(unit_);
        const bool is_card = entity->tags.contains(card_);
        const bool is_obstacle = entity->tags.contains(obstacle_);
        if (!is_unit && !is_card && !is_obstacle) {
            continue;
        }
        if (is_card && entity->owner != viewer) {
            continue;
        }
        if (is_unit && !unit_is_visible(state, *entity, viewer, unit_, health_, q_, r_)) {
            continue;
        }
        const auto kind = string(entity->properties, kind_).value_or("unknown");
        auto center = center_for(state, *entity->location, q_, r_, battlefield_space_count);
        if (is_card) {
            center.x += (static_cast<float>(id.index() % 3U) - 1.0F) * 0.18F;
        }
        const auto health = integer(entity->properties, health_);
        const float alpha = health && *health <= 0 ? 0.35F : 1.0F;
        const Color tint = is_obstacle
                               ? Color{1.0F, 1.0F, 1.0F, alpha}
                               : is_card
                                     ? Color{0.92F, 0.72F, 0.25F, alpha}
                                     : entity->owner == vanguard_player
                                           ? Color{0.33F, 0.73F, 0.95F, alpha}
                                           : Color{0.91F, 0.30F, 0.29F, alpha};
        result.pieces.push_back(PieceVisual{
            id, *entity->location, center,
            is_card ? Vec2{0.48F, 0.64F}
                    : is_obstacle ? Vec2{1.22F, 1.22F} : Vec2{1.08F, 1.42F},
            sprite_for(kind, is_card, is_obstacle), tint, is_card ? 3.0F : 2.0F, 0.0F,
            is_unit && entity->owner == viewer ? Color{0.62F, 0.83F, 1.0F, 0.72F}
                                                : Color{},
            is_unit && entity->owner == viewer ? 0.025F : 0.0F});
        if (is_unit && health) {
            result.bars.push_back(BarVisual{
                {{center.x - 0.48F, center.y + 0.69F},
                 {center.x + 0.48F, center.y + 0.79F}},
                static_cast<float>(*health), static_cast<float>(maximum_health(kind)),
                {0.025F, 0.03F, 0.04F, 0.92F},
                entity->owner == vanguard_player ? Color{0.22F, 0.72F, 0.94F, 1.0F}
                                                  : Color{0.78F, 0.22F, 0.28F, 1.0F},
                8.0F});
            result.texts.push_back(TextVisual{display_name(kind),
                                              {center.x, center.y + 0.98F}, 10.0F,
                                              {0.85F, 0.82F, 0.72F, 0.92F}, 9.0F, false});
            if (entity->tags.contains(poisoned_)) {
                result.effects.push_back(EffectVisual{
                    EffectKind::poison, center, {center.x, center.y - 0.3F},
                    {0.35F, 0.92F, 0.34F, 0.78F}, 0.42F,
                    std::chrono::steady_clock::now(), std::chrono::milliseconds{800}, 7.0F,
                    EffectBlend::additive, 0.45F, 1.0F});
            }
        }
    }
    std::ranges::sort(result.pieces, {}, &PieceVisual::id);

    for (const auto& intent : game.legal_actions(viewer)) {
        if (!intent.actor || find_piece(result, *intent.actor) == nullptr) {
            continue;
        }
        if (intent.targets.empty()) {
            result.choices.push_back(ChoiceHint{encode_action_token(intent),
                                                "End activation"});
            continue;
        }
        const auto actor = state.entities().snapshot(*intent.actor);
        if (!actor || !actor->location) {
            continue;
        }
        std::optional<SpaceId> destination;
        auto visual_kind = ActionVisualKind::move;
        if (const auto* space = std::get_if<SpaceId>(&intent.targets.front())) {
            destination = *space;
        } else if (const auto* target = std::get_if<EntityId>(&intent.targets.front())) {
            const auto entity = state.entities().snapshot(*target);
            visual_kind = entity && entity->owner == actor->owner
                              ? ActionVisualKind::ability
                              : ActionVisualKind::attack;
            if (entity && entity->location && find_piece(result, *target) != nullptr) {
                destination = entity->location;
            }
        }
        if (destination) {
            std::string label;
            if (visual_kind == ActionVisualKind::attack) {
                label = "Choose ability";
            } else if (visual_kind == ActionVisualKind::ability) {
                label = "Support (1 AP)";
            } else {
                int cost = 1;
                if (const auto target_space = state.topology().space(*destination);
                    target_space) {
                    if (const auto difficult = state.symbols().tags.find("difficult");
                        difficult && (*target_space)->tags.contains(*difficult)) {
                        cost = 2;
                    }
                }
                const auto actor_kind = string(actor->properties, kind_).value_or("unit");
                const auto from_space = state.topology().space(*actor->location);
                const auto target_space = state.topology().space(*destination);
                const auto coordinate = [&](const Space& space) {
                    return AxialCoord{
                        static_cast<int>(integer(space.properties, q_).value_or(0)),
                        static_cast<int>(integer(space.properties, r_).value_or(0))};
                };
                const bool dash = actor_kind == "stalker" && from_space && target_space &&
                                  hex_distance(coordinate(**from_space),
                                               coordinate(**target_space)) == 2;
                label = dash ? "Dash (" : "Move (";
                label += std::to_string(cost) + " AP)";
            }
            result.actions.push_back(ActionHint{encode_action_token(intent), *intent.actor,
                                                *actor->location,
                                                *destination, intent.type.value(), visual_kind,
                                                std::move(label),
                                                std::nullopt,
                                                visual_kind == ActionVisualKind::move
                                                    ? ActionTargetSemantics::space
                                                    : ActionTargetSemantics::entity});
        }
    }

    const auto& pending = state.effect_stack().pending_choice();
    if (pending) {
        result.choices.clear();
        result.choices.reserve(pending->options.size());
        for (const auto& option : pending->options) {
            result.choices.push_back(
                ChoiceHint{encode_choice_token(pending->id, option.id), option.label});
        }
        result.status = (pending->player == vanguard_player ? "Vanguard" : "Raiders") +
                        std::string{" — choose an ability (effect stack paused)"};
    } else {
        const auto metadata = game.entity_named("battle_state");
        if (!metadata) {
            return std::unexpected(metadata.error());
        }
        const auto snapshot = state.entities().snapshot(*metadata);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        const auto active = integer(snapshot->properties, active_player_);
        const auto round = integer(snapshot->properties, round_);
        const auto ap = action_points_ ? integer(snapshot->properties, *action_points_)
                                       : std::nullopt;
        const auto first_score = vanguard_score_
                                     ? integer(snapshot->properties, *vanguard_score_)
                                     : std::nullopt;
        const auto second_score = raiders_score_
                                      ? integer(snapshot->properties, *raiders_score_)
                                      : std::nullopt;
        const auto outcome = outcome_ ? string(snapshot->properties, *outcome_)
                                      : std::optional<std::string_view>{};
        std::string active_name = active && *active == 0 ? "Vanguard" : "Raiders";
        if (active_unit_) {
            const auto active_index = integer(snapshot->properties, *active_unit_);
            if (active_index && *active_index >= 0) {
                for (const auto id : state.entities().entities()) {
                    if (id.index() != static_cast<std::uint32_t>(*active_index)) {
                        continue;
                    }
                    const auto entity = state.entities().snapshot(id);
                    if (entity) {
                        active_name = display_name(string(entity->properties, kind_).value_or("unit"));
                    }
                    break;
                }
            }
        }
        if (outcome && *outcome != "ongoing") {
            const auto winner = *outcome == "vanguard" ? "Vanguard victory"
                                : *outcome == "raiders" ? "Raiders victory"
                                                        : "Draw";
            result.status = winner;
            result.texts.push_back(TextVisual{winner, {0.5F, 0.16F}, 34.0F,
                                              {0.92F, 0.82F, 0.56F, 1.0F}, 50.0F, true});
        } else {
            result.status = active_name + " — " + std::to_string(ap.value_or(1)) +
                            " AP — round " + std::to_string(round.value_or(0));
            if (first_score && second_score) {
                result.status += " — shrine " + std::to_string(*first_score) + ":" +
                                 std::to_string(*second_score) + " (first to 3)";
            }
        }
    }
    return result;
}

std::expected<PlayerView, Diagnostic>
TacticalPresentation::build_view(const TacticalGame& game, PlayerId viewer,
                                 std::uint64_t revision) const {
    auto render = build(game, viewer, revision);
    if (!render) {
        return std::unexpected(render.error());
    }
    PlayerView view;
    view.render = std::move(*render);
    const auto& state = game.session().state();
    const auto& symbols = state.symbols();
    const auto optional_property = [&](std::string_view name) -> std::optional<PropertyId> {
        const auto found = symbols.properties.find(name);
        return found ? std::optional<PropertyId>{*found} : std::nullopt;
    };
    const auto optional_tag = [&](std::string_view name) -> std::optional<TagId> {
        const auto found = symbols.tags.find(name);
        return found ? std::optional<TagId>{*found} : std::nullopt;
    };
    const auto initiative_id = optional_property("initiative");
    const auto armor_bonus_id = optional_property("armor_bonus");
    const auto guarded = optional_tag("guarded");
    const auto warded = optional_tag("warded");
    const auto bulwark = optional_tag("bulwark");

    std::optional<EntitySnapshot> battle_state;
    if (const auto metadata = game.entity_named("battle_state"); metadata) {
        const auto snapshot = state.entities().snapshot(*metadata);
        if (snapshot) {
            battle_state = *snapshot;
        }
    }
    std::optional<std::int64_t> active_index;
    std::optional<std::int64_t> active_ap;
    if (battle_state) {
        if (active_unit_) {
            active_index = integer(battle_state->properties, *active_unit_);
        }
        if (action_points_) {
            active_ap = integer(battle_state->properties, *action_points_);
        }
    }

    for (const auto id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(id);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        if (!entity->tags.contains(unit_) ||
            !unit_is_visible(state, *entity, viewer, unit_, health_, q_, r_)) {
            continue;
        }
        const auto kind = string(entity->properties, kind_).value_or("unknown");
        const auto health = integer(entity->properties, health_).value_or(0);
        std::vector<std::string> statuses;
        if (entity->tags.contains(poisoned_)) {
            statuses.emplace_back("Poisoned");
        }
        if (guarded && entity->tags.contains(*guarded)) {
            statuses.emplace_back("Guard");
        }
        if (warded && entity->tags.contains(*warded)) {
            statuses.emplace_back("Ward");
        }
        if (bulwark && entity->tags.contains(*bulwark)) {
            statuses.emplace_back("Bulwark");
        }
        if (armor_bonus_id && integer(entity->properties, *armor_bonus_id).value_or(0) > 0 &&
            statuses.empty()) {
            statuses.emplace_back("Armored");
        }
        const bool active = active_index && *active_index >= 0 &&
                            id.index() == static_cast<std::uint32_t>(*active_index);
        view.units.push_back(UnitCardView{
            id, display_name(kind),
            entity->owner == vanguard_player ? "Vanguard" : "Raiders", health,
            maximum_health(kind), active ? active_ap.value_or(0) : 0,
            sprite_for(kind, false, false), std::move(statuses)});
        view.initiative.push_back(InitiativeView{
            id, display_name(kind),
            initiative_id ? integer(entity->properties, *initiative_id).value_or(0) : 0,
            active, health <= 0});
    }
    std::ranges::sort(view.units, {}, &UnitCardView::entity);
    std::ranges::sort(view.initiative, [](const InitiativeView& left,
                                          const InitiativeView& right) {
        return left.initiative == right.initiative ? left.entity < right.entity
                                                    : left.initiative > right.initiative;
    });

    if (battle_state) {
        view.objective = ObjectiveScoreView{
            "Vanguard", "Raiders",
            vanguard_score_ ? integer(battle_state->properties, *vanguard_score_).value_or(0)
                             : 0,
            raiders_score_ ? integer(battle_state->properties, *raiders_score_).value_or(0)
                            : 0,
            3};
        if (outcome_) {
            const auto outcome = string(battle_state->properties, *outcome_);
            if (outcome && *outcome != "ongoing") {
                view.end_state = EndStateView{
                    *outcome == "vanguard" ? "Vanguard victory"
                    : *outcome == "raiders" ? "Raiders victory"
                                             : "Draw",
                    "The Shattered Shrine battle is complete", *outcome == "draw"};
            }
        }
    }
    view.abilities.reserve(view.render.actions.size() + view.render.choices.size());
    for (const auto& action : view.render.actions) {
        view.abilities.push_back(AbilityView{
            action.token, action.label,
            action.target == ActionTargetSemantics::entity ? "Choose a visible unit"
                                                            : "Choose a highlighted hex",
            1, action.icon, true});
    }
    for (const auto& choice : view.render.choices) {
        const auto cost = choice.label.find("Focused Shot") != std::string::npos ||
                                  choice.label.find("Drain") != std::string::npos
                              ? 2
                          : choice.label.find("End activation") != std::string::npos ? 0
                                                                                     : 1;
        view.abilities.push_back(
            AbilityView{choice.token, choice.label, "Resolve the pending ability", cost,
                        std::nullopt, true});
    }
    const auto history = game.action_history();
    const auto first = history.size() > 12U ? history.size() - 12U : 0U;
    for (std::size_t index = first; index < history.size(); ++index) {
        view.combat_log.push_back(CombatLogView{
            static_cast<std::uint64_t>(index + 1U),
            "Recorded action " + std::to_string(index + 1U),
            {0.82F, 0.84F, 0.9F, 1.0F}});
    }
    return view;
}

} // namespace ludus::tactical
