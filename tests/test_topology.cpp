#include "ludus/core/symbol.hpp"
#include "ludus/topology/topology.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

namespace {

ludus::RectangularDirections directions(ludus::SymbolTable<ludus::DirectionId>& symbols) {
    return {symbols.intern("north"), symbols.intern("east"), symbols.intern("south"),
            symbols.intern("west")};
}

} // namespace

TEST_CASE("rectangular grids are directed graphs with deterministic adjacency",
          "[topology][grid]") {
    ludus::SymbolTable<ludus::DirectionId> symbols;
    const auto grid = ludus::make_rectangular_grid(3U, 2U, directions(symbols));

    REQUIRE(grid);
    REQUIRE(grid->spaces().size() == 6U);
    REQUIRE(grid->links().size() == 14U);
    REQUIRE(grid->outgoing(ludus::rectangular_space_id(0U, 0U, 3U)).size() == 2U);
    REQUIRE(grid->outgoing(ludus::rectangular_space_id(1U, 0U, 3U)).size() == 3U);

    const auto center_links = grid->outgoing(ludus::rectangular_space_id(1U, 0U, 3U));
    REQUIRE(std::ranges::is_sorted(center_links, {}, &ludus::Link::direction));
    REQUIRE(grid->outgoing(ludus::SpaceId{99U, 1U}).empty());
}

TEST_CASE("irregular graph links support cost, direction, tags, and one-way travel",
          "[topology][graph]") {
    ludus::TopologyBuilder builder;
    const auto first = builder.add_space();
    const auto second = builder.add_space();
    const auto portal = ludus::DirectionId{1U};
    ludus::TagSet tags;
    REQUIRE(tags.add(ludus::TagId{1U}));
    REQUIRE(builder.add_link(first, second, portal, 7, tags));

    const auto graph = std::move(builder).build();
    REQUIRE(graph);
    REQUIRE(graph->outgoing(first).size() == 1U);
    REQUIRE(graph->outgoing(second).empty());
    REQUIRE(graph->outgoing(first).front().cost == 7);
    REQUIRE(graph->outgoing(first).front().tags.contains(ludus::TagId{1U}));
}

TEST_CASE("invalid rectangular dimensions and links are rejected", "[topology][validation]") {
    ludus::SymbolTable<ludus::DirectionId> symbols;
    REQUIRE_FALSE(ludus::make_rectangular_grid(0U, 8U, directions(symbols)));

    ludus::TopologyBuilder builder;
    const auto only = builder.add_space();
    REQUIRE_FALSE(builder.add_link(only, ludus::SpaceId{8U, 1U}, ludus::DirectionId{1U}));
    REQUIRE_FALSE(builder.add_link(only, only, ludus::DirectionId{1U}, -1));
}
