#include "ludus/topology/topology.hpp"

#include <algorithm>
#include <concepts>
#include <limits>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace ludus {
namespace {

void write_id(BinaryWriter& writer, SpaceId id) {
    writer.u32(id.index());
    writer.u32(id.generation());
}

SpaceId read_space_id(BinaryReader& reader) { return {reader.u32(), reader.u32()}; }

void write_tags(BinaryWriter& writer, const TagSet& tags) {
    writer.u64(static_cast<std::uint64_t>(tags.values().size()));
    for (const auto tag : tags.values()) {
        writer.u32(tag.value());
    }
}

std::expected<TagSet, Diagnostic> read_tags(BinaryReader& reader) {
    TagSet result;
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        const TagId tag{reader.u32()};
        if (!tag.valid() || !result.add(tag)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid serialized topology tag set", {}});
        }
    }
    return result;
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
                                          "unknown serialized property value kind", {}});
    }
}

void write_properties(BinaryWriter& writer, const PropertySet& properties) {
    writer.u64(static_cast<std::uint64_t>(properties.entries().size()));
    for (const auto& entry : properties.entries()) {
        writer.u32(entry.id.value());
        write_value(writer, entry.value);
    }
}

std::expected<PropertySet, Diagnostic> read_properties(BinaryReader& reader) {
    PropertySet result;
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        const PropertyId id{reader.u32()};
        auto value = read_value(reader);
        if (!id.valid() || !value || result.find(id) != nullptr) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid serialized topology properties", {}});
        }
        static_cast<void>(result.set(id, std::move(*value)));
    }
    return result;
}

} // namespace

bool Topology::contains(SpaceId id) const noexcept {
    return id.valid() && id.generation() == 1U && id.index() < spaces_.size();
}

std::expected<const Space*, Diagnostic> Topology::space(SpaceId id) const {
    if (!contains(id)) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_handle,
                                          "space handle is stale or invalid", {}});
    }
    return &spaces_[id.index()];
}

std::span<const Link> Topology::outgoing(SpaceId from) const noexcept {
    if (!contains(from)) {
        return {};
    }
    const auto begin = adjacency_offsets_[from.index()];
    const auto end = adjacency_offsets_[from.index() + 1U];
    return std::span<const Link>{links_}.subspan(begin, end - begin);
}

SpaceId TopologyBuilder::add_space(TagSet tags, PropertySet properties) {
    const SpaceId id{static_cast<std::uint32_t>(spaces_.size()), 1U};
    spaces_.push_back(Space{id, std::move(tags), std::move(properties)});
    return id;
}

std::expected<void, Diagnostic> TopologyBuilder::add_link(SpaceId from, SpaceId to,
                                                          DirectionId direction,
                                                          std::int32_t cost, TagSet tags) {
    const auto valid_space = [this](SpaceId id) {
        return id.valid() && id.generation() == 1U && id.index() < spaces_.size();
    };
    if (!valid_space(from) || !valid_space(to)) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_handle,
                                          "topology link references an invalid space", {}});
    }
    if (!direction.valid()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "topology link direction is invalid", {}});
    }
    if (cost < 0) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "topology link cost cannot be negative", {}});
    }
    links_.push_back(Link{from, to, direction, cost, std::move(tags)});
    return {};
}

std::expected<Topology, Diagnostic> TopologyBuilder::build() && {
    std::ranges::sort(links_, [](const Link& left, const Link& right) {
        const auto left_key = std::tie(left.from, left.direction, left.to, left.cost);
        const auto right_key = std::tie(right.from, right.direction, right.to, right.cost);
        if (left_key != right_key) {
            return left_key < right_key;
        }
        return left.tags < right.tags;
    });
    if (std::ranges::adjacent_find(links_) != links_.end()) {
        return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                          "topology contains a duplicate directed link", {}});
    }

    Topology result;
    result.spaces_ = std::move(spaces_);
    result.links_ = std::move(links_);
    result.adjacency_offsets_.assign(result.spaces_.size() + 1U, 0U);
    for (const auto& link : result.links_) {
        ++result.adjacency_offsets_[link.from.index() + 1U];
    }
    for (std::size_t index = 1; index < result.adjacency_offsets_.size(); ++index) {
        result.adjacency_offsets_[index] += result.adjacency_offsets_[index - 1U];
    }
    return result;
}

std::expected<Topology, Diagnostic>
make_rectangular_grid(std::uint32_t width, std::uint32_t height,
                      RectangularDirections directions) {
    if (width == 0U || height == 0U ||
        width > std::numeric_limits<std::uint32_t>::max() / height) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "rectangular grid dimensions are invalid", {}});
    }
    if (!directions.north.valid() || !directions.east.valid() ||
        !directions.south.valid() || !directions.west.valid()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "rectangular grid directions must be interned", {}});
    }

    TopologyBuilder builder;
    for (std::uint32_t index = 0; index < width * height; ++index) {
        static_cast<void>(builder.add_space());
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto from = rectangular_space_id(x, y, width);
            if (y > 0U) {
                static_cast<void>(builder.add_link(from, rectangular_space_id(x, y - 1U, width),
                                                   directions.north));
            }
            if (x + 1U < width) {
                static_cast<void>(builder.add_link(from, rectangular_space_id(x + 1U, y, width),
                                                   directions.east));
            }
            if (y + 1U < height) {
                static_cast<void>(builder.add_link(from, rectangular_space_id(x, y + 1U, width),
                                                   directions.south));
            }
            if (x > 0U) {
                static_cast<void>(builder.add_link(from, rectangular_space_id(x - 1U, y, width),
                                                   directions.west));
            }
        }
    }
    return std::move(builder).build();
}

void Topology::encode(BinaryWriter& writer) const {
    writer.u64(static_cast<std::uint64_t>(spaces_.size()));
    for (const auto& item : spaces_) {
        write_id(writer, item.id);
        write_tags(writer, item.tags);
        write_properties(writer, item.properties);
    }
    writer.u64(static_cast<std::uint64_t>(links_.size()));
    for (const auto& link : links_) {
        write_id(writer, link.from);
        write_id(writer, link.to);
        writer.u32(link.direction.value());
        writer.i32(link.cost);
        write_tags(writer, link.tags);
    }
}

std::expected<Topology, Diagnostic> Topology::decode(BinaryReader& reader) {
    TopologyBuilder builder;
    const auto space_count = reader.u64();
    if (space_count > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "too many serialized spaces", {}});
    }
    for (std::uint64_t index = 0; index < space_count && reader.ok(); ++index) {
        const auto id = read_space_id(reader);
        auto tags = read_tags(reader);
        auto properties = read_properties(reader);
        if (!tags || !properties || id != SpaceId{static_cast<std::uint32_t>(index), 1U}) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid serialized space", {}});
        }
        static_cast<void>(builder.add_space(std::move(*tags), std::move(*properties)));
    }
    const auto link_count = reader.u64();
    for (std::uint64_t index = 0; index < link_count && reader.ok(); ++index) {
        const auto from = read_space_id(reader);
        const auto to = read_space_id(reader);
        const DirectionId direction{reader.u32()};
        const auto cost = reader.i32();
        auto tags = read_tags(reader);
        if (!tags) {
            return std::unexpected(tags.error());
        }
        auto added = builder.add_link(from, to, direction, cost, std::move(*tags));
        if (!added) {
            return std::unexpected(added.error());
        }
    }
    if (!reader.ok()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          std::string{reader.error()}, {}});
    }
    return std::move(builder).build();
}

} // namespace ludus
