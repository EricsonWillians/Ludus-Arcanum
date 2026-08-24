#include "ludus/rules/event.hpp"

#include <algorithm>
#include <concepts>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace ludus {
namespace {

void write_id(BinaryWriter& writer, auto id) {
    writer.u32(id.index());
    writer.u32(id.generation());
}

template <typename Id>
Id read_id(BinaryReader& reader) {
    return Id{reader.u32(), reader.u32()};
}

template <typename Id>
void write_optional_id(BinaryWriter& writer, std::optional<Id> id) {
    writer.boolean(id.has_value());
    if (id) {
        write_id(writer, *id);
    }
}

template <typename Id>
std::optional<Id> read_optional_id(BinaryReader& reader) {
    if (!reader.boolean()) {
        return std::nullopt;
    }
    return read_id<Id>(reader);
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

std::expected<PropertyValue, Diagnostic> read_value(BinaryReader& reader) {
    switch (reader.u8()) {
    case 0:
        return PropertyValue{reader.boolean()};
    case 1:
        return PropertyValue{reader.i64()};
    case 2:
        return PropertyValue{Fixed::from_raw(reader.i64())};
    case 3:
        return PropertyValue{reader.string()};
    default:
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "unknown event property value kind", {}});
    }
}

void write_optional_value(BinaryWriter& writer, const std::optional<PropertyValue>& value) {
    writer.boolean(value.has_value());
    if (value) {
        write_value(writer, *value);
    }
}

std::expected<std::optional<PropertyValue>, Diagnostic>
read_optional_value(BinaryReader& reader) {
    if (!reader.boolean()) {
        return std::optional<PropertyValue>{};
    }
    auto value = read_value(reader);
    if (!value) {
        return std::unexpected(value.error());
    }
    return std::optional<PropertyValue>{std::move(*value)};
}

void write_entity(BinaryWriter& writer, const EntitySnapshot& entity) {
    write_id(writer, entity.id);
    write_optional_id(writer, entity.location);
    write_optional_id(writer, entity.owner);
    writer.u64(static_cast<std::uint64_t>(entity.tags.values().size()));
    for (const auto tag : entity.tags.values()) {
        writer.u32(tag.value());
    }
    writer.u64(static_cast<std::uint64_t>(entity.properties.entries().size()));
    for (const auto& property : entity.properties.entries()) {
        writer.u32(property.id.value());
        write_value(writer, property.value);
    }
}

std::expected<EntitySnapshot, Diagnostic> read_entity(BinaryReader& reader) {
    EntitySnapshot entity;
    entity.id = read_id<EntityId>(reader);
    entity.location = read_optional_id<SpaceId>(reader);
    entity.owner = read_optional_id<PlayerId>(reader);
    const auto tag_count = reader.u64();
    for (std::uint64_t index = 0; index < tag_count && reader.ok(); ++index) {
        const TagId tag{reader.u32()};
        if (!tag.valid() || !entity.tags.add(tag)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid event entity tags", {}});
        }
    }
    const auto property_count = reader.u64();
    for (std::uint64_t index = 0; index < property_count && reader.ok(); ++index) {
        const PropertyId property{reader.u32()};
        auto value = read_value(reader);
        if (!property.valid() || !value || entity.properties.find(property) != nullptr) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid event entity properties", {}});
        }
        static_cast<void>(entity.properties.set(property, std::move(*value)));
    }
    if (!entity.id.valid() || !reader.ok()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "invalid event entity snapshot", {}});
    }
    return entity;
}

} // namespace

void encode_event(BinaryWriter& writer, const Event& event) {
    writer.u64(event.sequence);
    writer.u8(static_cast<std::uint8_t>(event.payload.index()));
    std::visit(
        [&writer](const auto& payload) {
            using T = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<T, EntitySpawned> || std::same_as<T, EntityDestroyed>) {
                write_entity(writer, payload.entity);
            } else if constexpr (std::same_as<T, EntityMoved>) {
                write_id(writer, payload.entity);
                write_optional_id(writer, payload.from);
                write_optional_id(writer, payload.to);
            } else if constexpr (std::same_as<T, EntityOwnerChanged>) {
                write_id(writer, payload.entity);
                write_optional_id(writer, payload.from);
                write_optional_id(writer, payload.to);
            } else if constexpr (std::same_as<T, EntityPropertyChanged>) {
                write_id(writer, payload.entity);
                writer.u32(payload.property.value());
                write_optional_value(writer, payload.from);
                write_optional_value(writer, payload.to);
            } else if constexpr (std::same_as<T, EntityTagChanged>) {
                write_id(writer, payload.entity);
                writer.u32(payload.tag.value());
                writer.boolean(payload.added);
            } else if constexpr (std::same_as<T, DiceRolled>) {
                writer.string(payload.result.stream);
                writer.string(payload.result.expression);
                writer.u64(static_cast<std::uint64_t>(payload.result.dice.size()));
                for (const auto die : payload.result.dice) {
                    writer.u32(die);
                }
                writer.i64(payload.result.total);
            } else if constexpr (std::same_as<T, EffectPushed> ||
                                 std::same_as<T, EffectPopped>) {
                encode_effect_record(writer, payload.effect);
            } else if constexpr (std::same_as<T, ChoiceRequested>) {
                encode_choice_window(writer, payload.choice);
            } else if constexpr (std::same_as<T, ChoiceResolved>) {
                encode_choice_window(writer, payload.choice);
                writer.u32(payload.option_id);
            }
        },
        event.payload);
}

std::expected<Event, Diagnostic> decode_event(BinaryReader& reader) {
    const auto sequence = reader.u64();
    const auto kind = reader.u8();
    EventPayload payload;
    switch (kind) {
    case 0: {
        auto entity = read_entity(reader);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        payload = EntitySpawned{std::move(*entity)};
        break;
    }
    case 1: {
        auto entity = read_entity(reader);
        if (!entity) {
            return std::unexpected(entity.error());
        }
        payload = EntityDestroyed{std::move(*entity)};
        break;
    }
    case 2:
        payload = EntityMoved{read_id<EntityId>(reader), read_optional_id<SpaceId>(reader),
                              read_optional_id<SpaceId>(reader)};
        break;
    case 3:
        payload = EntityOwnerChanged{read_id<EntityId>(reader),
                                     read_optional_id<PlayerId>(reader),
                                     read_optional_id<PlayerId>(reader)};
        break;
    case 4: {
        const auto entity = read_id<EntityId>(reader);
        const PropertyId property{reader.u32()};
        auto before = read_optional_value(reader);
        auto after = read_optional_value(reader);
        if (!before || !after) {
            return std::unexpected(before ? after.error() : before.error());
        }
        payload = EntityPropertyChanged{entity, property, std::move(*before), std::move(*after)};
        break;
    }
    case 5:
        payload = EntityTagChanged{read_id<EntityId>(reader), TagId{reader.u32()},
                                   reader.boolean()};
        break;
    case 6: {
        DiceResult result;
        result.stream = reader.string();
        result.expression = reader.string();
        const auto count = reader.u64();
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "too many recorded dice outcomes", {}});
        }
        result.dice.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
            result.dice.push_back(reader.u32());
        }
        result.total = reader.i64();
        payload = DiceRolled{std::move(result)};
        break;
    }
    case 7: {
        auto effect = decode_effect_record(reader);
        if (!effect) {
            return std::unexpected(effect.error());
        }
        payload = EffectPushed{std::move(*effect)};
        break;
    }
    case 8: {
        auto effect = decode_effect_record(reader);
        if (!effect) {
            return std::unexpected(effect.error());
        }
        payload = EffectPopped{std::move(*effect)};
        break;
    }
    case 9: {
        auto choice = decode_choice_window(reader);
        if (!choice) {
            return std::unexpected(choice.error());
        }
        payload = ChoiceRequested{std::move(*choice)};
        break;
    }
    case 10: {
        auto choice = decode_choice_window(reader);
        if (!choice) {
            return std::unexpected(choice.error());
        }
        const auto option_id = reader.u32();
        if (std::ranges::none_of(choice->options, [option_id](const ChoiceOption& option) {
                return option.id == option_id;
            })) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "resolved choice option is invalid", {}});
        }
        payload = ChoiceResolved{std::move(*choice), option_id};
        break;
    }
    default:
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "unknown serialized event kind", {}});
    }
    if (sequence == 0U || !reader.ok()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "invalid serialized event", {}});
    }
    return Event{sequence, std::move(payload)};
}

} // namespace ludus
