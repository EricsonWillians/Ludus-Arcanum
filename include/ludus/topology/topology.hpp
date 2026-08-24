#pragma once

#include "ludus/core/binary.hpp"
#include "ludus/core/diagnostic.hpp"
#include "ludus/core/id.hpp"
#include "ludus/core/value.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ludus {

struct Space {
    SpaceId id;
    TagSet tags;
    PropertySet properties;

    auto operator<=>(const Space&) const = default;
};

struct Link {
    SpaceId from;
    SpaceId to;
    DirectionId direction;
    std::int32_t cost{1};
    TagSet tags;

    auto operator<=>(const Link&) const = default;
};

class Topology {
  public:
    [[nodiscard]] bool contains(SpaceId id) const noexcept;
    [[nodiscard]] std::expected<const Space*, Diagnostic> space(SpaceId id) const;
    [[nodiscard]] std::span<const Space> spaces() const noexcept { return spaces_; }
    [[nodiscard]] std::span<const Link> links() const noexcept { return links_; }
    [[nodiscard]] std::span<const Link> outgoing(SpaceId from) const noexcept;

    void encode(BinaryWriter& writer) const;
    [[nodiscard]] static std::expected<Topology, Diagnostic> decode(BinaryReader& reader);

    auto operator<=>(const Topology&) const = default;

  private:
    std::vector<Space> spaces_;
    std::vector<Link> links_;
    std::vector<std::size_t> adjacency_offsets_{0U};

    friend class TopologyBuilder;
};

class TopologyBuilder {
  public:
    [[nodiscard]] SpaceId add_space(TagSet tags = {}, PropertySet properties = {});
    [[nodiscard]] std::expected<void, Diagnostic>
    add_link(SpaceId from, SpaceId to, DirectionId direction, std::int32_t cost = 1,
             TagSet tags = {});
    [[nodiscard]] std::expected<Topology, Diagnostic> build() &&;

  private:
    std::vector<Space> spaces_;
    std::vector<Link> links_;
};

struct RectangularDirections {
    DirectionId north;
    DirectionId east;
    DirectionId south;
    DirectionId west;
};

[[nodiscard]] std::expected<Topology, Diagnostic>
make_rectangular_grid(std::uint32_t width, std::uint32_t height,
                      RectangularDirections directions);

[[nodiscard]] constexpr SpaceId rectangular_space_id(std::uint32_t x, std::uint32_t y,
                                                      std::uint32_t width) noexcept {
    return SpaceId{y * width + x, 1U};
}

} // namespace ludus
