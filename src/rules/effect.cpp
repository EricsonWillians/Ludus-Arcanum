#include "ludus/rules/effect.hpp"

#include <algorithm>
#include <concepts>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace ludus {
namespace {

Diagnostic invalid_effect(std::string message) {
    return Diagnostic{DiagnosticCode::serialization_error, std::move(message), {}};
}

void write_id(BinaryWriter& writer, auto id) {
    writer.u32(id.index());
    writer.u32(id.generation());
}

template <typename Id>
Id read_id(BinaryReader& reader) {
    return Id{reader.u32(), reader.u32()};
}

void write_value(BinaryWriter& writer, const PropertyValue& value) {
    writer.u8(static_cast<std::uint8_t>(value.index()));
    std::visit(
        [&writer](const auto& typed) {
            using Value = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::same_as<Value, bool>) {
                writer.boolean(typed);
            } else if constexpr (std::same_as<Value, std::int64_t>) {
                writer.i64(typed);
            } else if constexpr (std::same_as<Value, Fixed>) {
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
        return std::unexpected(invalid_effect("unknown effect argument value kind"));
    }
}

void write_properties(BinaryWriter& writer, const PropertySet& properties) {
    writer.u64(static_cast<std::uint64_t>(properties.entries().size()));
    for (const auto& property : properties.entries()) {
        writer.u32(property.id.value());
        write_value(writer, property.value);
    }
}

std::expected<PropertySet, Diagnostic> read_properties(BinaryReader& reader) {
    const auto count = reader.u64();
    if (count > EffectStack::maximum_arguments) {
        return std::unexpected(invalid_effect("effect argument count exceeds its limit"));
    }
    PropertySet result;
    for (std::uint64_t index = 0U; index < count && reader.ok(); ++index) {
        const PropertyId property{reader.u32()};
        auto value = read_value(reader);
        if (!property.valid() || !value || result.find(property) != nullptr) {
            return std::unexpected(invalid_effect("invalid effect arguments"));
        }
        static_cast<void>(result.set(property, std::move(*value)));
    }
    return result;
}

std::expected<void, Diagnostic> validate_effect_shape(const EffectRecord& effect) {
    if (effect.id == 0U || !effect.continuation.valid() ||
        (effect.source && !effect.source->valid()) ||
        effect.entity_targets.size() > EffectStack::maximum_targets ||
        effect.space_targets.size() > EffectStack::maximum_targets ||
        effect.arguments.entries().size() > EffectStack::maximum_arguments ||
        std::ranges::any_of(effect.entity_targets,
                            [](EntityId id) { return !id.valid(); }) ||
        std::ranges::any_of(effect.space_targets,
                            [](SpaceId id) { return !id.valid(); })) {
        return std::unexpected(invalid_effect("effect record is malformed"));
    }
    return {};
}

std::expected<void, Diagnostic> validate_choice_shape(const ChoiceWindow& choice) {
    if (choice.id == 0U || !choice.player.valid() || choice.prompt.empty() ||
        choice.prompt.size() > EffectStack::maximum_text_length || choice.options.empty() ||
        choice.options.size() > EffectStack::maximum_choice_options) {
        return std::unexpected(invalid_effect("choice window is malformed"));
    }
    std::set<std::uint32_t> option_ids;
    for (const auto& option : choice.options) {
        if (option.id == 0U || option.label.empty() ||
            option.label.size() > EffectStack::maximum_text_length ||
            option.arguments.entries().size() > EffectStack::maximum_arguments ||
            !option_ids.insert(option.id).second) {
            return std::unexpected(invalid_effect("choice option is malformed or duplicated"));
        }
    }
    return {};
}

} // namespace

void encode_effect_record(BinaryWriter& writer, const EffectRecord& effect) {
    writer.u64(effect.id);
    writer.u32(effect.continuation.value());
    writer.boolean(effect.source.has_value());
    if (effect.source) {
        write_id(writer, *effect.source);
    }
    writer.u64(static_cast<std::uint64_t>(effect.entity_targets.size()));
    for (const auto target : effect.entity_targets) {
        write_id(writer, target);
    }
    writer.u64(static_cast<std::uint64_t>(effect.space_targets.size()));
    for (const auto target : effect.space_targets) {
        write_id(writer, target);
    }
    write_properties(writer, effect.arguments);
}

std::expected<EffectRecord, Diagnostic> decode_effect_record(BinaryReader& reader) {
    EffectRecord result;
    result.id = reader.u64();
    result.continuation = ActionTypeId{reader.u32()};
    if (reader.boolean()) {
        result.source = read_id<EntityId>(reader);
    }
    const auto entity_count = reader.u64();
    if (entity_count > EffectStack::maximum_targets) {
        return std::unexpected(invalid_effect("effect entity target limit exceeded"));
    }
    result.entity_targets.reserve(static_cast<std::size_t>(entity_count));
    for (std::uint64_t index = 0U; index < entity_count && reader.ok(); ++index) {
        result.entity_targets.push_back(read_id<EntityId>(reader));
    }
    const auto space_count = reader.u64();
    if (space_count > EffectStack::maximum_targets) {
        return std::unexpected(invalid_effect("effect space target limit exceeded"));
    }
    result.space_targets.reserve(static_cast<std::size_t>(space_count));
    for (std::uint64_t index = 0U; index < space_count && reader.ok(); ++index) {
        result.space_targets.push_back(read_id<SpaceId>(reader));
    }
    auto arguments = read_properties(reader);
    if (!arguments) {
        return std::unexpected(arguments.error());
    }
    result.arguments = std::move(*arguments);
    if (!reader.ok()) {
        return std::unexpected(invalid_effect(std::string{reader.error()}));
    }
    if (auto valid = validate_effect_shape(result); !valid) {
        return std::unexpected(valid.error());
    }
    return result;
}

void encode_choice_window(BinaryWriter& writer, const ChoiceWindow& choice) {
    writer.u64(choice.id);
    write_id(writer, choice.player);
    writer.string(choice.prompt);
    writer.u64(static_cast<std::uint64_t>(choice.options.size()));
    for (const auto& option : choice.options) {
        writer.u32(option.id);
        writer.string(option.label);
        write_properties(writer, option.arguments);
    }
}

std::expected<ChoiceWindow, Diagnostic> decode_choice_window(BinaryReader& reader) {
    ChoiceWindow result;
    result.id = reader.u64();
    result.player = read_id<PlayerId>(reader);
    result.prompt = reader.string();
    const auto count = reader.u64();
    if (count > EffectStack::maximum_choice_options) {
        return std::unexpected(invalid_effect("choice option limit exceeded"));
    }
    result.options.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count && reader.ok(); ++index) {
        ChoiceOption option;
        option.id = reader.u32();
        option.label = reader.string();
        auto arguments = read_properties(reader);
        if (!arguments) {
            return std::unexpected(arguments.error());
        }
        option.arguments = std::move(*arguments);
        result.options.push_back(std::move(option));
    }
    if (!reader.ok()) {
        return std::unexpected(invalid_effect(std::string{reader.error()}));
    }
    if (auto valid = validate_choice_shape(result); !valid) {
        return std::unexpected(valid.error());
    }
    return result;
}

void EffectStack::encode(BinaryWriter& writer) const {
    writer.u64(static_cast<std::uint64_t>(effects_.size()));
    for (const auto& effect : effects_) {
        encode_effect_record(writer, effect);
    }
    writer.boolean(pending_choice_.has_value());
    if (pending_choice_) {
        encode_choice_window(writer, *pending_choice_);
    }
}

std::expected<EffectStack, Diagnostic> EffectStack::decode(BinaryReader& reader) {
    const auto count = reader.u64();
    if (count > maximum_depth) {
        return std::unexpected(invalid_effect("effect stack depth limit exceeded"));
    }
    EffectStack result;
    result.effects_.reserve(static_cast<std::size_t>(count));
    std::set<std::uint64_t> ids;
    for (std::uint64_t index = 0U; index < count && reader.ok(); ++index) {
        auto effect = decode_effect_record(reader);
        if (!effect || !ids.insert(effect->id).second) {
            return std::unexpected(effect ? invalid_effect("duplicate effect identifier")
                                          : effect.error());
        }
        result.effects_.push_back(std::move(*effect));
    }
    if (reader.boolean()) {
        auto choice = decode_choice_window(reader);
        if (!choice || result.effects_.empty() ||
            (choice && choice->id != result.effects_.back().id)) {
            return std::unexpected(choice ? invalid_effect("choice requires a pending effect")
                                          : choice.error());
        }
        result.pending_choice_ = std::move(*choice);
    }
    if (!reader.ok()) {
        return std::unexpected(invalid_effect(std::string{reader.error()}));
    }
    return result;
}

} // namespace ludus
