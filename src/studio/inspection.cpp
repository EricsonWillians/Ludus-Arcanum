#include "ludus/studio/inspection.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

namespace ludus::studio {
namespace {

std::string stable_id(auto id) {
    if (!id.valid()) {
        return "none";
    }
    return std::to_string(id.index()) + ':' + std::to_string(id.generation());
}

std::string optional_id(const auto& id) {
    return id ? stable_id(*id) : std::string{"none"};
}

std::string property_value(const PropertyValue& value) {
    return std::visit(
        [](const auto& typed) {
            using Value = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::same_as<Value, bool>) {
                return std::string{typed ? "true" : "false"};
            } else if constexpr (std::same_as<Value, std::int64_t>) {
                return std::to_string(typed);
            } else if constexpr (std::same_as<Value, Fixed>) {
                return std::to_string(typed.raw()) + "e-4";
            } else {
                return '"' + typed + '"';
            }
        },
        value);
}

std::string property_name(PropertyId id, const SymbolRegistry& symbols) {
    const auto name = symbols.properties.name(id);
    return name ? std::string{*name} : std::string{"property#"} +
                                               std::to_string(id.value());
}

std::string event_line(const Event& event, const SymbolRegistry& symbols) {
    std::ostringstream output;
    output << '#' << event.sequence << ' ';
    std::visit(
        [&output, &symbols](const auto& payload) {
            using Payload = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<Payload, EntitySpawned>) {
                output << "EntitySpawned entity=" << stable_id(payload.entity.id)
                       << " location=" << optional_id(payload.entity.location);
            } else if constexpr (std::same_as<Payload, EntityDestroyed>) {
                output << "EntityDestroyed entity=" << stable_id(payload.entity.id)
                       << " location=" << optional_id(payload.entity.location);
            } else if constexpr (std::same_as<Payload, EntityMoved>) {
                output << "EntityMoved entity=" << stable_id(payload.entity)
                       << " from=" << optional_id(payload.from)
                       << " to=" << optional_id(payload.to);
            } else if constexpr (std::same_as<Payload, EntityOwnerChanged>) {
                output << "EntityOwnerChanged entity=" << stable_id(payload.entity)
                       << " from=" << optional_id(payload.from)
                       << " to=" << optional_id(payload.to);
            } else if constexpr (std::same_as<Payload, EntityPropertyChanged>) {
                output << "EntityPropertyChanged entity=" << stable_id(payload.entity)
                       << " property=" << property_name(payload.property, symbols);
                if (payload.from) {
                    output << " from=" << property_value(*payload.from);
                }
                if (payload.to) {
                    output << " to=" << property_value(*payload.to);
                }
            } else if constexpr (std::same_as<Payload, EntityTagChanged>) {
                const auto name = symbols.tags.name(payload.tag);
                output << "EntityTagChanged entity=" << stable_id(payload.entity)
                       << " tag="
                       << (name ? std::string{*name}
                                : std::string{"tag#"} + std::to_string(payload.tag.value()))
                       << (payload.added ? " added" : " removed");
            } else if constexpr (std::same_as<Payload, DiceRolled>) {
                output << "DiceRolled stream=" << payload.result.stream
                       << " expression=" << payload.result.expression
                       << " total=" << payload.result.total;
            } else if constexpr (std::same_as<Payload, EffectPushed>) {
                output << "EffectPushed id=" << payload.effect.id
                       << " continuation=" << payload.effect.continuation.value();
            } else if constexpr (std::same_as<Payload, EffectPopped>) {
                output << "EffectPopped id=" << payload.effect.id
                       << " continuation=" << payload.effect.continuation.value();
            } else if constexpr (std::same_as<Payload, ChoiceRequested>) {
                output << "ChoiceRequested id=" << payload.choice.id
                       << " player=" << stable_id(payload.choice.player)
                       << " options=" << payload.choice.options.size();
            } else if constexpr (std::same_as<Payload, ChoiceResolved>) {
                output << "ChoiceResolved id=" << payload.choice.id
                       << " option=" << payload.option_id;
            }
        },
        event.payload);
    return output.str();
}

} // namespace

std::string inspect_state(const GameState& state) {
    std::ostringstream output;
    output << "State hash: " << std::hex << state.canonical_hash() << std::dec << '\n'
           << "Spaces: " << state.topology().spaces().size() << '\n'
           << "Directed links: " << state.topology().links().size() << '\n'
           << "Entities: " << state.entities().size() << '\n'
           << "Effects: " << state.effect_stack().effects().size() << '\n';
    if (const auto& choice = state.effect_stack().pending_choice(); choice) {
        output << "Pending choice: " << choice->id << " for " << stable_id(choice->player)
               << " (" << choice->options.size() << " options)\n";
    }
    output << '\n';
    for (const auto id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(id);
        if (!entity) {
            output << "entity " << stable_id(id) << " <invalid>\n";
            continue;
        }
        output << "entity " << stable_id(id) << " location="
               << optional_id(entity->location) << " owner=" << optional_id(entity->owner)
               << '\n';
        if (!entity->tags.values().empty()) {
            output << "  tags:";
            for (const auto tag : entity->tags.values()) {
                const auto name = state.symbols().tags.name(tag);
                output << ' '
                       << (name ? std::string{*name}
                                : std::string{"tag#"} + std::to_string(tag.value()));
            }
            output << '\n';
        }
        for (const auto& property : entity->properties.entries()) {
            output << "  " << property_name(property.id, state.symbols()) << " = "
                   << property_value(property.value) << '\n';
        }
    }
    return output.str();
}

std::string inspect_event_log(std::span<const EventBatch> batches, std::size_t cursor,
                              const SymbolRegistry& symbols) {
    std::ostringstream output;
    const auto committed = std::min(cursor, batches.size());
    for (std::size_t index = 0U; index < batches.size(); ++index) {
        output << (index < committed ? "[applied] " : "[redo] ") << "batch " << index + 1U
               << " hash=" << std::hex << batches[index].resulting_state_hash << std::dec
               << '\n';
        for (const auto& event : batches[index].events) {
            output << "  " << event_line(event, symbols) << '\n';
        }
    }
    if (batches.empty()) {
        output << "No committed playtest events.\n";
    }
    return output.str();
}

} // namespace ludus::studio
