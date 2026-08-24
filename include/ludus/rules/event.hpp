#pragma once

#include "ludus/core/binary.hpp"
#include "ludus/core/diagnostic.hpp"
#include "ludus/core/entity_store.hpp"
#include "ludus/core/id.hpp"
#include "ludus/core/value.hpp"
#include "ludus/rules/effect.hpp"
#include "ludus/rules/random.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <variant>
#include <vector>

namespace ludus {

struct EntitySpawned {
    EntitySnapshot entity;
    auto operator<=>(const EntitySpawned&) const = default;
};

struct EntityDestroyed {
    EntitySnapshot entity;
    auto operator<=>(const EntityDestroyed&) const = default;
};

struct EntityMoved {
    EntityId entity;
    std::optional<SpaceId> from;
    std::optional<SpaceId> to;
    auto operator<=>(const EntityMoved&) const = default;
};

struct EntityOwnerChanged {
    EntityId entity;
    std::optional<PlayerId> from;
    std::optional<PlayerId> to;
    auto operator<=>(const EntityOwnerChanged&) const = default;
};

struct EntityPropertyChanged {
    EntityId entity;
    PropertyId property;
    std::optional<PropertyValue> from;
    std::optional<PropertyValue> to;
    auto operator<=>(const EntityPropertyChanged&) const = default;
};

struct EntityTagChanged {
    EntityId entity;
    TagId tag;
    bool added{false};
    auto operator<=>(const EntityTagChanged&) const = default;
};

struct DiceRolled {
    DiceResult result;
    auto operator<=>(const DiceRolled&) const = default;
};

struct EffectPushed {
    EffectRecord effect;
    auto operator<=>(const EffectPushed&) const = default;
};

struct EffectPopped {
    EffectRecord effect;
    auto operator<=>(const EffectPopped&) const = default;
};

struct ChoiceRequested {
    ChoiceWindow choice;
    auto operator<=>(const ChoiceRequested&) const = default;
};

struct ChoiceResolved {
    ChoiceWindow choice;
    std::uint32_t option_id{0U};
    auto operator<=>(const ChoiceResolved&) const = default;
};

using EventPayload = std::variant<EntitySpawned, EntityDestroyed, EntityMoved,
                                  EntityOwnerChanged, EntityPropertyChanged, EntityTagChanged,
                                  DiceRolled, EffectPushed, EffectPopped, ChoiceRequested,
                                  ChoiceResolved>;

struct Event {
    std::uint64_t sequence{0};
    EventPayload payload;

    auto operator<=>(const Event&) const = default;
};

struct EventBatch {
    std::vector<Event> events;
    std::uint64_t resulting_state_hash{0};

    auto operator<=>(const EventBatch&) const = default;
};

void encode_event(BinaryWriter& writer, const Event& event);
[[nodiscard]] std::expected<Event, Diagnostic> decode_event(BinaryReader& reader);

} // namespace ludus
