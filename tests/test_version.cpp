#include "ludus/core/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("the runtime exposes a semantic project version", "[foundation]") {
    const auto version = ludus::version();

    REQUIRE_FALSE(version.empty());
    REQUIRE(version == std::string_view{"0.1.0"});
}
