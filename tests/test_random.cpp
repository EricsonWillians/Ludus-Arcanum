#include "ludus/rules/random.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

TEST_CASE("PCG32 version 1 has stable documented vectors", "[random][determinism]") {
    ludus::DeterministicRandom random{42U};
    constexpr std::array<std::uint32_t, 8> expected{
        792'947'071U, 114'436'514U, 3'312'788'359U, 3'403'716'857U,
        2'234'156'308U, 353'647'627U, 841'742'849U, 3'452'942'987U};

    for (const auto value : expected) {
        REQUIRE(random.next_u32("combat") == value);
    }
    STATIC_REQUIRE(ludus::DeterministicRandom::algorithm_version == 1U);
}

TEST_CASE("named random streams do not perturb one another", "[random][streams]") {
    ludus::DeterministicRandom interleaved{9U};
    const auto combat_first = interleaved.next_u32("combat");
    static_cast<void>(interleaved.next_u32("loot"));
    const auto combat_second = interleaved.next_u32("combat");

    ludus::DeterministicRandom isolated{9U};
    REQUIRE(isolated.next_u32("combat") == combat_first);
    REQUIRE(isolated.next_u32("combat") == combat_second);
}

TEST_CASE("dice expressions record raw outcomes and totals", "[random][dice]") {
    ludus::DeterministicRandom random{42U};
    const auto ordinary = random.roll("2d6+3", "combat");

    REQUIRE(ordinary);
    REQUIRE(ordinary->dice == std::vector<std::uint32_t>{2U, 3U});
    REQUIRE(ordinary->total == 8);
    REQUIRE(ordinary->expression == "2d6+3");
    REQUIRE(ordinary->stream == "combat");

    const auto advantage = random.roll("2d20kh1+4", "combat");
    REQUIRE(advantage);
    REQUIRE(advantage->dice.size() == 2U);
    REQUIRE(advantage->total ==
            static_cast<std::int64_t>(std::max(advantage->dice[0], advantage->dice[1])) + 4);

    const auto exploding = random.roll("1d2!", "explosions");
    REQUIRE(exploding);
    REQUIRE_FALSE(exploding->dice.empty());
    REQUIRE(exploding->total >= 1);
    REQUIRE_FALSE(random.roll("not dice", "combat"));
}

TEST_CASE("random state round trips canonically", "[random][serialization]") {
    ludus::DeterministicRandom original{123'456U};
    static_cast<void>(original.next_u32("combat"));
    static_cast<void>(original.next_u32("cards"));

    ludus::BinaryWriter writer;
    original.encode(writer);
    ludus::BinaryReader reader{writer.data()};
    auto restored = ludus::DeterministicRandom::decode(reader);

    REQUIRE(restored);
    REQUIRE(reader.at_end());
    REQUIRE(restored->next_u32("combat") == original.next_u32("combat"));
    REQUIRE(restored->next_u32("cards") == original.next_u32("cards"));
}

TEST_CASE("random snapshot continuation property holds across seeds and draw counts",
          "[random][property]") {
    for (std::uint64_t seed = 0U; seed < 64U; ++seed) {
        ludus::DeterministicRandom original{seed};
        for (std::uint64_t draw = 0U; draw < seed; ++draw) {
            static_cast<void>(original.next_u32(draw % 2U == 0U ? "even" : "odd"));
        }
        auto restored = ludus::DeterministicRandom{seed};
        restored.restore(original.snapshot());
        for (std::size_t continuation = 0U; continuation < 16U; ++continuation) {
            const auto stream = continuation % 3U == 0U ? "new" : "even";
            REQUIRE(restored.next_u32(stream) == original.next_u32(stream));
        }
    }
}
