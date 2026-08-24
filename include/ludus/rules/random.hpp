#pragma once

#include "ludus/core/binary.hpp"
#include "ludus/core/diagnostic.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ludus {

struct RandomStreamState {
    std::uint64_t state{0};
    std::uint64_t increment{0};
    std::uint64_t draws{0};

    auto operator<=>(const RandomStreamState&) const = default;
};

struct RandomSnapshot {
    std::map<std::string, RandomStreamState, std::less<>> streams;

    auto operator<=>(const RandomSnapshot&) const = default;
};

struct DiceResult {
    std::string stream;
    std::string expression;
    std::vector<std::uint32_t> dice;
    std::int64_t total{0};

    auto operator<=>(const DiceResult&) const = default;
};

class DeterministicRandom {
  public:
    static constexpr std::uint32_t algorithm_version = 1U;

    explicit DeterministicRandom(std::uint64_t master_seed = 0U) : master_seed_(master_seed) {}

    [[nodiscard]] std::uint64_t master_seed() const noexcept { return master_seed_; }
    [[nodiscard]] std::uint32_t next_u32(std::string_view stream);
    [[nodiscard]] std::uint32_t uniform(std::string_view stream, std::uint32_t bound);
    [[nodiscard]] std::expected<DiceResult, Diagnostic> roll(std::string_view expression,
                                                            std::string_view stream);

    [[nodiscard]] RandomSnapshot snapshot() const { return RandomSnapshot{streams_}; }
    void restore(const RandomSnapshot& snapshot) { streams_ = snapshot.streams; }

    void encode(BinaryWriter& writer) const;
    [[nodiscard]] static std::expected<DeterministicRandom, Diagnostic>
    decode(BinaryReader& reader);

  private:
    [[nodiscard]] RandomStreamState make_stream(std::string_view name) const;
    [[nodiscard]] static std::uint32_t advance(RandomStreamState& stream) noexcept;

    std::uint64_t master_seed_{0};
    std::map<std::string, RandomStreamState, std::less<>> streams_;
};

} // namespace ludus
