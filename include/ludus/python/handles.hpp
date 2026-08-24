#pragma once

#include "ludus/core/id.hpp"

namespace ludus {

/// Python-visible value handle. It contains identity, never a storage address.
struct EntityHandle {
    EntityId id;
    auto operator<=>(const EntityHandle&) const = default;
};

struct SpaceHandle {
    SpaceId id;
    auto operator<=>(const SpaceHandle&) const = default;
};

struct PlayerHandle {
    PlayerId id;
    auto operator<=>(const PlayerHandle&) const = default;
};

struct ActionHandle {
    ActionTypeId id;
    auto operator<=>(const ActionHandle&) const = default;
};

} // namespace ludus
