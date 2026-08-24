#include "ludus/core/entity_store.hpp"

#include <algorithm>
#include <concepts>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace ludus {
namespace {

void write_stable_id(BinaryWriter& writer, auto id) {
    writer.u32(id.index());
    writer.u32(id.generation());
}

template <typename Id>
Id read_stable_id(BinaryReader& reader) {
    return Id{reader.u32(), reader.u32()};
}

void write_optional_space(BinaryWriter& writer, std::optional<SpaceId> value) {
    writer.boolean(value.has_value());
    if (value) {
        write_stable_id(writer, *value);
    }
}

void write_optional_player(BinaryWriter& writer, std::optional<PlayerId> value) {
    writer.boolean(value.has_value());
    if (value) {
        write_stable_id(writer, *value);
    }
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
            } else if constexpr (std::same_as<T, std::string>) {
                writer.string(typed);
            }
        },
        value);
}

PropertyValue read_value(BinaryReader& reader) {
    switch (reader.u8()) {
    case 0:
        return reader.boolean();
    case 1:
        return reader.i64();
    case 2:
        return Fixed::from_raw(reader.i64());
    case 3:
        return reader.string();
    default:
        reader.invalidate("unknown serialized property value kind");
        return false;
    }
}

void write_tags(BinaryWriter& writer, const TagSet& tags) {
    writer.u64(static_cast<std::uint64_t>(tags.values().size()));
    for (const auto tag : tags.values()) {
        writer.u32(tag.value());
    }
}

TagSet read_tags(BinaryReader& reader) {
    TagSet tags;
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        const TagId tag{reader.u32()};
        if (!tag.valid() || !tags.add(tag)) {
            reader.invalidate("invalid serialized entity tag set");
            break;
        }
    }
    return tags;
}

void write_properties(BinaryWriter& writer, const PropertySet& properties) {
    writer.u64(static_cast<std::uint64_t>(properties.entries().size()));
    for (const auto& property : properties.entries()) {
        writer.u32(property.id.value());
        write_value(writer, property.value);
    }
}

PropertySet read_properties(BinaryReader& reader) {
    PropertySet properties;
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        const PropertyId id{reader.u32()};
        auto value = read_value(reader);
        if (!id.valid() || properties.find(id) != nullptr) {
            reader.invalidate("invalid serialized entity property set");
            break;
        }
        static_cast<void>(properties.set(id, std::move(value)));
    }
    return properties;
}

} // namespace

bool EntityStore::contains(EntityId id) const noexcept {
    return id.valid() && id.index() < slots_.size() && slots_[id.index()].alive &&
           slots_[id.index()].generation == id.generation();
}

std::expected<const EntityStore::Slot*, Diagnostic> EntityStore::slot(EntityId id) const {
    if (!contains(id)) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_handle,
                                          "entity handle is stale or invalid", {}});
    }
    return &slots_[id.index()];
}

std::expected<EntityStore::Slot*, Diagnostic> EntityStore::slot(EntityId id) {
    if (!contains(id)) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_handle,
                                          "entity handle is stale or invalid", {}});
    }
    return &slots_[id.index()];
}

std::expected<EntitySnapshot, Diagnostic> EntityStore::snapshot(EntityId id) const {
    const auto found = slot(id);
    if (!found) {
        return std::unexpected(found.error());
    }
    return EntitySnapshot{id, (*found)->location, (*found)->owner, (*found)->tags,
                          (*found)->properties};
}

std::uint32_t EntityStore::next_generation(std::uint32_t current) noexcept {
    const auto next = current + 1U;
    return next == 0U ? 1U : next;
}

EntityStore::SpawnAllocation EntityStore::spawn(EntitySnapshot snapshot) {
    std::uint32_t index{0};
    bool appended = false;
    if (free_slots_.empty()) {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
        appended = true;
    } else {
        index = *free_slots_.begin();
        free_slots_.erase(free_slots_.begin());
    }

    auto& target = slots_[index];
    target.alive = true;
    target.location = snapshot.location;
    target.owner = snapshot.owner;
    target.tags = std::move(snapshot.tags);
    target.properties = std::move(snapshot.properties);
    const EntityId id{index, target.generation};
    rebuild_alive_ids();
    return SpawnAllocation{id, appended};
}

EntitySnapshot EntityStore::destroy(EntityId id) {
    auto& target = slots_[id.index()];
    EntitySnapshot previous{id, target.location, target.owner, std::move(target.tags),
                            std::move(target.properties)};
    target.location.reset();
    target.owner.reset();
    target.alive = false;
    target.generation = next_generation(target.generation);
    free_slots_.insert(id.index());
    rebuild_alive_ids();
    return previous;
}

void EntityStore::undo_spawn(EntityId id, bool appended) {
    if (appended) {
        slots_.pop_back();
    } else {
        auto& target = slots_[id.index()];
        target = Slot{};
        target.generation = id.generation();
        free_slots_.insert(id.index());
    }
    rebuild_alive_ids();
}

void EntityStore::redo_spawn(const EntitySnapshot& snapshot, bool appended) {
    if (appended) {
        slots_.emplace_back();
        slots_.back().generation = snapshot.id.generation();
    } else {
        free_slots_.erase(snapshot.id.index());
    }
    auto& target = slots_[snapshot.id.index()];
    target.alive = true;
    target.location = snapshot.location;
    target.owner = snapshot.owner;
    target.tags = snapshot.tags;
    target.properties = snapshot.properties;
    rebuild_alive_ids();
}

void EntityStore::undo_destroy(const EntitySnapshot& snapshot) {
    free_slots_.erase(snapshot.id.index());
    auto& target = slots_[snapshot.id.index()];
    target.generation = snapshot.id.generation();
    target.alive = true;
    target.location = snapshot.location;
    target.owner = snapshot.owner;
    target.tags = snapshot.tags;
    target.properties = snapshot.properties;
    rebuild_alive_ids();
}

void EntityStore::redo_destroy(EntityId id) { static_cast<void>(destroy(id)); }

void EntityStore::rebuild_alive_ids() {
    alive_ids_.clear();
    alive_ids_.reserve(slots_.size() - free_slots_.size());
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const auto& candidate = slots_[index];
        if (candidate.alive) {
            alive_ids_.emplace_back(static_cast<std::uint32_t>(index), candidate.generation);
        }
    }
}

void EntityStore::encode(BinaryWriter& writer) const {
    writer.u64(static_cast<std::uint64_t>(slots_.size()));
    for (const auto& entity : slots_) {
        writer.u32(entity.generation);
        writer.boolean(entity.alive);
        if (entity.alive) {
            write_optional_space(writer, entity.location);
            write_optional_player(writer, entity.owner);
            write_tags(writer, entity.tags);
            write_properties(writer, entity.properties);
        }
    }
}

std::expected<EntityStore, Diagnostic> EntityStore::decode(BinaryReader& reader) {
    EntityStore result;
    const auto count = reader.u64();
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "too many serialized entity slots", {}});
    }
    result.slots_.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        Slot entity;
        entity.generation = reader.u32();
        entity.alive = reader.boolean();
        if (entity.generation == 0U) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "entity generation cannot be zero", {}});
        }
        if (entity.alive) {
            if (reader.boolean()) {
                entity.location = read_stable_id<SpaceId>(reader);
            }
            if (reader.boolean()) {
                entity.owner = read_stable_id<PlayerId>(reader);
            }
            entity.tags = read_tags(reader);
            entity.properties = read_properties(reader);
        } else {
            result.free_slots_.insert(static_cast<std::uint32_t>(index));
        }
        result.slots_.push_back(std::move(entity));
    }
    if (!reader.ok()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          std::string{reader.error()}, {}});
    }
    result.rebuild_alive_ids();
    return result;
}

} // namespace ludus
