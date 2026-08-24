#include "ludus/rules/game_state.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace ludus {
namespace {

constexpr std::string_view state_magic = "LUDUS-STATE";
constexpr std::uint32_t state_version = 2U;

template <typename Id>
void write_symbols(BinaryWriter& writer, const SymbolTable<Id>& table) {
    writer.u64(static_cast<std::uint64_t>(table.size()));
    for (const auto& name : table.names()) {
        writer.string(name);
    }
}

template <typename Id>
std::expected<SymbolTable<Id>, Diagnostic> read_symbols(BinaryReader& reader) {
    const auto count = reader.u64();
    if (count > std::numeric_limits<typename Id::value_type>::max() - 1ULL) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "serialized symbol table is too large", {}});
    }
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        names.push_back(reader.string());
    }
    auto table = SymbolTable<Id>::from_names(names);
    if (!table) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error, table.error(), {}});
    }
    return std::move(*table);
}

bool symbol_exists(std::size_t size, auto id) {
    return id.valid() && id.value() <= size;
}

std::expected<void, Diagnostic> validate(const GameState& state) {
    for (const auto& space : state.topology().spaces()) {
        for (const auto tag : space.tags.values()) {
            if (!symbol_exists(state.symbols().tags.size(), tag)) {
                return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                  "space references an unknown tag", {}});
            }
        }
        for (const auto& property : space.properties.entries()) {
            if (!symbol_exists(state.symbols().properties.size(), property.id)) {
                return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                  "space references an unknown property", {}});
            }
        }
    }
    for (const auto& link : state.topology().links()) {
        if (!symbol_exists(state.symbols().directions.size(), link.direction)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "link references an unknown direction", {}});
        }
        for (const auto tag : link.tags.values()) {
            if (!symbol_exists(state.symbols().tags.size(), tag)) {
                return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                  "link references an unknown tag", {}});
            }
        }
    }
    for (const auto entity_id : state.entities().entities()) {
        const auto entity = state.entities().snapshot(entity_id);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        if (entity->location && !state.topology().contains(*entity->location)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "entity references an unknown space", {}});
        }
        if (entity->owner && !entity->owner->valid()) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "entity has an invalid owner", {}});
        }
        for (const auto tag : entity->tags.values()) {
            if (!symbol_exists(state.symbols().tags.size(), tag)) {
                return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                  "entity references an unknown tag", {}});
            }
        }
        for (const auto& property : entity->properties.entries()) {
            if (!symbol_exists(state.symbols().properties.size(), property.id)) {
                return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                  "entity references an unknown property", {}});
            }
        }
    }
    for (const auto& effect : state.effect_stack().effects()) {
        if (!symbol_exists(state.symbols().actions.size(), effect.continuation) ||
            (effect.source && !state.entities().contains(*effect.source)) ||
            std::ranges::any_of(effect.entity_targets, [&state](EntityId target) {
                return !state.entities().contains(target);
            }) ||
            std::ranges::any_of(effect.space_targets, [&state](SpaceId target) {
                return !state.topology().contains(target);
            })) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "effect references invalid state", {}});
        }
        for (const auto& argument : effect.arguments.entries()) {
            if (!symbol_exists(state.symbols().properties.size(), argument.id)) {
                return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                  "effect references an unknown property", {}});
            }
        }
    }
    if (const auto& choice = state.effect_stack().pending_choice(); choice) {
        for (const auto& option : choice->options) {
            for (const auto& argument : option.arguments.entries()) {
                if (!symbol_exists(state.symbols().properties.size(), argument.id)) {
                    return std::unexpected(Diagnostic{
                        DiagnosticCode::serialization_error,
                        "choice option references an unknown property", {}});
                }
            }
        }
    }
    return {};
}

} // namespace

std::vector<std::byte> GameState::canonical_bytes() const {
    BinaryWriter writer;
    writer.string(state_magic);
    writer.u32(state_version);
    write_symbols(writer, symbols_.tags);
    write_symbols(writer, symbols_.properties);
    write_symbols(writer, symbols_.directions);
    write_symbols(writer, symbols_.actions);
    write_symbols(writer, symbols_.events);
    topology_.encode(writer);
    entities_.encode(writer);
    effect_stack_.encode(writer);
    return std::move(writer).take();
}

std::uint64_t GameState::canonical_hash() const {
    const auto bytes = canonical_bytes();
    return ludus::canonical_hash(bytes);
}

std::expected<GameState, Diagnostic>
GameState::from_canonical_bytes(std::span<const std::byte> bytes) {
    BinaryReader reader{bytes};
    const auto magic = reader.string();
    const auto version = reader.u32();
    if (magic != state_magic || (version != 1U && version != state_version)) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "unsupported or invalid state header", {}});
    }

    auto tags = read_symbols<TagId>(reader);
    auto properties = read_symbols<PropertyId>(reader);
    auto directions = read_symbols<DirectionId>(reader);
    auto actions = read_symbols<ActionTypeId>(reader);
    auto events = read_symbols<EventTypeId>(reader);
    if (!tags || !properties || !directions || !actions || !events) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "invalid state symbol registry", {}});
    }
    auto topology = Topology::decode(reader);
    auto entities = EntityStore::decode(reader);
    if (!topology) {
        return std::unexpected(topology.error());
    }
    if (!entities) {
        return std::unexpected(entities.error());
    }
    std::expected<EffectStack, Diagnostic> effects{EffectStack{}};
    if (version >= 2U) {
        effects = EffectStack::decode(reader);
        if (!effects) {
            return std::unexpected(effects.error());
        }
    }
    if (!reader.ok() || !reader.at_end()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          reader.ok() ? "trailing canonical state data"
                                                      : std::string{reader.error()},
                                          {}});
    }

    GameState result;
    result.symbols_ = SymbolRegistry{std::move(*tags), std::move(*properties),
                                     std::move(*directions), std::move(*actions),
                                     std::move(*events)};
    result.topology_ = std::move(*topology);
    result.entities_ = std::move(*entities);
    result.effect_stack_ = std::move(*effects);
    if (auto checked = validate(result); !checked) {
        return std::unexpected(checked.error());
    }
    return result;
}

} // namespace ludus
