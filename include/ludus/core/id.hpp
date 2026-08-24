#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>

namespace ludus {

template <typename Domain>
class StableId {
  public:
    using value_type = std::uint32_t;

    constexpr StableId() noexcept = default;
    constexpr StableId(value_type index, value_type generation) noexcept
        : index_(index), generation_(generation) {}

    [[nodiscard]] constexpr value_type index() const noexcept { return index_; }
    [[nodiscard]] constexpr value_type generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index_ != invalid_index && generation_ != 0U;
    }
    [[nodiscard]] constexpr std::uint64_t packed() const noexcept {
        return (static_cast<std::uint64_t>(generation_) << 32U) | index_;
    }

    static constexpr value_type invalid_index = std::numeric_limits<value_type>::max();

    auto operator<=>(const StableId&) const = default;

  private:
    value_type index_{invalid_index};
    value_type generation_{0};
};

template <typename Domain>
class SymbolId {
  public:
    using value_type = std::uint32_t;

    constexpr SymbolId() noexcept = default;
    explicit constexpr SymbolId(value_type value) noexcept : value_(value) {}

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0U; }

    auto operator<=>(const SymbolId&) const = default;

  private:
    value_type value_{0};
};

struct EntityIdDomain;
struct SpaceIdDomain;
struct PlayerIdDomain;
struct TagIdDomain;
struct PropertyIdDomain;
struct DirectionIdDomain;
struct ActionTypeIdDomain;
struct EventTypeIdDomain;

using EntityId = StableId<EntityIdDomain>;
using SpaceId = StableId<SpaceIdDomain>;
using PlayerId = StableId<PlayerIdDomain>;
using TagId = SymbolId<TagIdDomain>;
using PropertyId = SymbolId<PropertyIdDomain>;
using DirectionId = SymbolId<DirectionIdDomain>;
using ActionTypeId = SymbolId<ActionTypeIdDomain>;
using EventTypeId = SymbolId<EventTypeIdDomain>;

} // namespace ludus

template <typename Domain>
struct std::hash<ludus::StableId<Domain>> {
    std::size_t operator()(ludus::StableId<Domain> id) const noexcept {
        return std::hash<std::uint64_t>{}(id.packed());
    }
};

template <typename Domain>
struct std::hash<ludus::SymbolId<Domain>> {
    std::size_t operator()(ludus::SymbolId<Domain> id) const noexcept {
        return std::hash<std::uint32_t>{}(id.value());
    }
};
