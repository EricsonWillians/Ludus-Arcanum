#pragma once

#include "ludus/core/id.hpp"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ludus {

class Fixed {
  public:
    static constexpr std::int64_t scale = 10'000;

    constexpr Fixed() noexcept = default;
    static constexpr Fixed from_raw(std::int64_t raw) noexcept { return Fixed{raw}; }
    static constexpr Fixed from_integer(std::int64_t integer) noexcept {
        return Fixed{integer * scale};
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
    auto operator<=>(const Fixed&) const = default;

  private:
    explicit constexpr Fixed(std::int64_t raw) noexcept : raw_(raw) {}
    std::int64_t raw_{0};
};

using PropertyValue = std::variant<bool, std::int64_t, Fixed, std::string>;

struct PropertyEntry {
    PropertyId id;
    PropertyValue value;

    auto operator<=>(const PropertyEntry&) const = default;
};

class PropertySet {
  public:
    [[nodiscard]] const PropertyValue* find(PropertyId id) const noexcept {
        const auto found = lower_bound(id);
        return found != entries_.end() && found->id == id ? &found->value : nullptr;
    }

    [[nodiscard]] std::optional<PropertyValue> set(PropertyId id, PropertyValue value) {
        auto found = lower_bound(id);
        if (found != entries_.end() && found->id == id) {
            found->value.swap(value);
            return std::optional<PropertyValue>{std::move(value)};
        }
        entries_.insert(found, PropertyEntry{id, std::move(value)});
        return std::nullopt;
    }

    [[nodiscard]] std::optional<PropertyValue> erase(PropertyId id) {
        auto found = lower_bound(id);
        if (found == entries_.end() || found->id != id) {
            return std::nullopt;
        }
        auto previous = std::move(found->value);
        entries_.erase(found);
        return previous;
    }

    [[nodiscard]] std::span<const PropertyEntry> entries() const noexcept { return entries_; }

    auto operator<=>(const PropertySet&) const = default;

  private:
    using Iterator = std::vector<PropertyEntry>::iterator;
    using ConstIterator = std::vector<PropertyEntry>::const_iterator;

    [[nodiscard]] Iterator lower_bound(PropertyId id) noexcept {
        return std::lower_bound(entries_.begin(), entries_.end(), id,
                                [](const PropertyEntry& entry, PropertyId needle) {
                                    return entry.id < needle;
                                });
    }
    [[nodiscard]] ConstIterator lower_bound(PropertyId id) const noexcept {
        return std::lower_bound(entries_.begin(), entries_.end(), id,
                                [](const PropertyEntry& entry, PropertyId needle) {
                                    return entry.id < needle;
                                });
    }

    std::vector<PropertyEntry> entries_;
};

class TagSet {
  public:
    [[nodiscard]] bool contains(TagId id) const noexcept {
        return std::binary_search(tags_.begin(), tags_.end(), id);
    }
    [[nodiscard]] bool add(TagId id) {
        const auto found = std::lower_bound(tags_.begin(), tags_.end(), id);
        if (found != tags_.end() && *found == id) {
            return false;
        }
        tags_.insert(found, id);
        return true;
    }
    [[nodiscard]] bool remove(TagId id) {
        const auto found = std::lower_bound(tags_.begin(), tags_.end(), id);
        if (found == tags_.end() || *found != id) {
            return false;
        }
        tags_.erase(found);
        return true;
    }
    [[nodiscard]] std::span<const TagId> values() const noexcept { return tags_; }

    auto operator<=>(const TagSet&) const = default;

  private:
    std::vector<TagId> tags_;
};

} // namespace ludus
