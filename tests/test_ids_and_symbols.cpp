#include "ludus/core/id.hpp"
#include "ludus/core/symbol.hpp"
#include "ludus/core/value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

TEST_CASE("stable IDs preserve index and generation", "[core][ids]") {
    constexpr ludus::EntityId first{7U, 3U};
    constexpr ludus::EntityId next_generation{7U, 4U};

    STATIC_REQUIRE(first.valid());
    STATIC_REQUIRE(first.packed() == 0x0000000300000007ULL);
    STATIC_REQUIRE(first != next_generation);
    STATIC_REQUIRE_FALSE(ludus::EntityId{}.valid());
}

TEST_CASE("symbols are interned once and retain deterministic order", "[core][symbols]") {
    ludus::SymbolTable<ludus::TagId> tags;
    const auto blocker = tags.intern("blocker");
    const auto royal = tags.intern("royal");

    REQUIRE(tags.intern("blocker") == blocker);
    REQUIRE(blocker.value() == 1U);
    REQUIRE(royal.value() == 2U);
    REQUIRE(tags.name(royal) == "royal");
    REQUIRE_FALSE(tags.find("missing"));

    const auto restored = ludus::SymbolTable<ludus::TagId>::from_names(tags.names());
    REQUIRE(restored);
    REQUIRE(*restored == tags);
}

TEST_CASE("property and tag storage is typed, sorted, and deterministic", "[core][state]") {
    ludus::PropertySet properties;
    const ludus::PropertyId health{2U};
    const ludus::PropertyId title{1U};

    REQUIRE_FALSE(properties.set(health, std::int64_t{12}));
    REQUIRE_FALSE(properties.set(title, std::string{"sentinel"}));
    REQUIRE(properties.entries()[0].id == title);
    REQUIRE(properties.entries()[1].id == health);
    const auto* health_value = properties.find(health);
    REQUIRE(health_value != nullptr);
    if (health_value == nullptr) {
        return;
    }
    const auto* health_integer = std::get_if<std::int64_t>(health_value);
    REQUIRE(health_integer != nullptr);
    if (health_integer == nullptr) {
        return;
    }
    REQUIRE(*health_integer == 12);
    REQUIRE(properties.set(health, std::int64_t{9}) ==
            std::optional<ludus::PropertyValue>{std::int64_t{12}});

    ludus::TagSet tags;
    REQUIRE(tags.add(ludus::TagId{3U}));
    REQUIRE(tags.add(ludus::TagId{1U}));
    REQUIRE_FALSE(tags.add(ludus::TagId{1U}));
    REQUIRE(tags.values()[0] == ludus::TagId{1U});
    REQUIRE(tags.remove(ludus::TagId{3U}));
}
