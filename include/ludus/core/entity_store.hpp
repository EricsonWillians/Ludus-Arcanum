#pragma once

#include "ludus/core/binary.hpp"
#include "ludus/core/diagnostic.hpp"
#include "ludus/core/id.hpp"
#include "ludus/core/value.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace ludus {

class Transaction;
class GameSession;

struct EntitySnapshot {
    EntityId id;
    std::optional<SpaceId> location;
    std::optional<PlayerId> owner;
    TagSet tags;
    PropertySet properties;

    auto operator<=>(const EntitySnapshot&) const = default;
};

class EntityStore {
  public:
    [[nodiscard]] bool contains(EntityId id) const noexcept;
    [[nodiscard]] std::expected<EntitySnapshot, Diagnostic> snapshot(EntityId id) const;
    [[nodiscard]] std::span<const EntityId> entities() const noexcept { return alive_ids_; }
    [[nodiscard]] std::size_t size() const noexcept { return alive_ids_.size(); }

    void encode(BinaryWriter& writer) const;
    [[nodiscard]] static std::expected<EntityStore, Diagnostic> decode(BinaryReader& reader);

    auto operator<=>(const EntityStore&) const = default;

  private:
    struct Slot {
        std::uint32_t generation{1};
        bool alive{false};
        std::optional<SpaceId> location;
        std::optional<PlayerId> owner;
        TagSet tags;
        PropertySet properties;

        auto operator<=>(const Slot&) const = default;
    };

    struct SpawnAllocation {
        EntityId id;
        bool appended{false};
    };

    [[nodiscard]] std::expected<const Slot*, Diagnostic> slot(EntityId id) const;
    [[nodiscard]] std::expected<Slot*, Diagnostic> slot(EntityId id);
    [[nodiscard]] SpawnAllocation spawn(EntitySnapshot snapshot);
    [[nodiscard]] EntitySnapshot destroy(EntityId id);
    void undo_spawn(EntityId id, bool appended);
    void redo_spawn(const EntitySnapshot& snapshot, bool appended);
    void undo_destroy(const EntitySnapshot& snapshot);
    void redo_destroy(EntityId id);
    void rebuild_alive_ids();

    static std::uint32_t next_generation(std::uint32_t current) noexcept;

    std::vector<Slot> slots_;
    std::set<std::uint32_t> free_slots_;
    std::vector<EntityId> alive_ids_;

    friend class Transaction;
    friend class GameSession;
};

} // namespace ludus
