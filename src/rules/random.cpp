#include "ludus/rules/random.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace ludus {
namespace {

constexpr std::uint64_t multiplier = 6'364'136'223'846'793'005ULL;

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t name_hash(std::string_view name) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const auto character : name) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

struct ParsedDice {
    std::uint32_t count{1};
    std::uint32_t sides{0};
    bool exploding{false};
    enum class Keep { all, highest, lowest } keep{Keep::all};
    std::int64_t modifier{0};
};

bool parse_unsigned(std::string_view text, std::size_t& cursor, std::uint32_t& result) {
    const auto begin = cursor;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
        ++cursor;
    }
    if (cursor == begin) {
        return false;
    }
    const auto conversion = std::from_chars(text.data() + begin, text.data() + cursor, result);
    return conversion.ec == std::errc{};
}

std::expected<ParsedDice, Diagnostic> parse_dice(std::string_view expression) {
    std::string compact;
    compact.reserve(expression.size());
    for (const char character : expression) {
        if (std::isspace(static_cast<unsigned char>(character)) == 0) {
            compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
    }

    ParsedDice parsed;
    std::size_t cursor = 0;
    const auto before_count = cursor;
    if (!parse_unsigned(compact, cursor, parsed.count)) {
        parsed.count = 1U;
        cursor = before_count;
    }
    if (cursor >= compact.size() || compact[cursor] != 'd') {
        return std::unexpected(Diagnostic{DiagnosticCode::random_expression_error,
                                          "dice expression must contain 'd'", {}});
    }
    ++cursor;
    if (!parse_unsigned(compact, cursor, parsed.sides)) {
        return std::unexpected(Diagnostic{DiagnosticCode::random_expression_error,
                                          "dice expression is missing its side count", {}});
    }
    if (parsed.count == 0U || parsed.count > 1'000U || parsed.sides < 2U ||
        parsed.sides > 1'000'000U) {
        return std::unexpected(Diagnostic{DiagnosticCode::random_expression_error,
                                          "dice count or side count is outside supported limits", {}});
    }
    if (cursor < compact.size() && compact[cursor] == '!') {
        parsed.exploding = true;
        ++cursor;
    }
    if (compact.substr(cursor, 3) == "kh1") {
        parsed.keep = ParsedDice::Keep::highest;
        cursor += 3U;
    } else if (compact.substr(cursor, 3) == "kl1") {
        parsed.keep = ParsedDice::Keep::lowest;
        cursor += 3U;
    }
    if (cursor < compact.size() && (compact[cursor] == '+' || compact[cursor] == '-')) {
        const bool negative = compact[cursor] == '-';
        ++cursor;
        std::uint32_t magnitude{0};
        if (!parse_unsigned(compact, cursor, magnitude)) {
            return std::unexpected(Diagnostic{DiagnosticCode::random_expression_error,
                                              "dice modifier is missing its value", {}});
        }
        parsed.modifier = negative ? -static_cast<std::int64_t>(magnitude)
                                   : static_cast<std::int64_t>(magnitude);
    }
    if (cursor != compact.size()) {
        return std::unexpected(Diagnostic{DiagnosticCode::random_expression_error,
                                          "dice expression has an unsupported suffix", {}});
    }
    return parsed;
}

} // namespace

std::uint32_t DeterministicRandom::advance(RandomStreamState& stream) noexcept {
    const auto old_state = stream.state;
    stream.state = old_state * multiplier + stream.increment;
    ++stream.draws;
    const auto xor_shifted = static_cast<std::uint32_t>(((old_state >> 18U) ^ old_state) >> 27U);
    const auto rotation = static_cast<std::uint32_t>(old_state >> 59U);
    return (xor_shifted >> rotation) | (xor_shifted << ((0U - rotation) & 31U));
}

RandomStreamState DeterministicRandom::make_stream(std::string_view name) const {
    const auto hash = name_hash(name);
    const auto initial_state = splitmix64(master_seed_ ^ hash);
    const auto sequence = splitmix64(master_seed_ + hash);
    RandomStreamState stream{.state = 0U, .increment = (sequence << 1U) | 1U, .draws = 0U};
    static_cast<void>(advance(stream));
    stream.state += initial_state;
    static_cast<void>(advance(stream));
    stream.draws = 0U;
    return stream;
}

std::uint32_t DeterministicRandom::next_u32(std::string_view stream) {
    auto [found, inserted] = streams_.try_emplace(std::string{stream});
    if (inserted) {
        found->second = make_stream(stream);
    }
    return advance(found->second);
}

std::uint32_t DeterministicRandom::uniform(std::string_view stream, std::uint32_t bound) {
    if (bound == 0U) {
        return 0U;
    }
    const auto threshold = static_cast<std::uint32_t>(0U - bound) % bound;
    for (;;) {
        const auto value = next_u32(stream);
        if (value >= threshold) {
            return value % bound;
        }
    }
}

std::expected<DiceResult, Diagnostic> DeterministicRandom::roll(std::string_view expression,
                                                               std::string_view stream) {
    if (stream.empty()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "random stream name cannot be empty", {}});
    }
    const auto parsed = parse_dice(expression);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    DiceResult result{std::string{stream}, std::string{expression}, {}, parsed->modifier};
    result.dice.reserve(parsed->count);
    std::vector<std::int64_t> groups;
    groups.reserve(parsed->count);
    std::size_t outcomes = 0U;
    for (std::uint32_t index = 0; index < parsed->count; ++index) {
        std::int64_t group_total{0};
        std::uint32_t face{0};
        do {
            if (++outcomes > 10'000U) {
                return std::unexpected(Diagnostic{DiagnosticCode::random_expression_error,
                                                  "exploding dice exceeded the outcome limit", {}});
            }
            face = uniform(stream, parsed->sides) + 1U;
            result.dice.push_back(face);
            group_total += face;
        } while (parsed->exploding && face == parsed->sides);
        groups.push_back(group_total);
    }

    if (parsed->keep == ParsedDice::Keep::highest) {
        result.total += *std::ranges::max_element(groups);
    } else if (parsed->keep == ParsedDice::Keep::lowest) {
        result.total += *std::ranges::min_element(groups);
    } else {
        for (const auto group : groups) {
            result.total += group;
        }
    }
    return result;
}

void DeterministicRandom::encode(BinaryWriter& writer) const {
    writer.u32(algorithm_version);
    writer.u64(master_seed_);
    writer.u64(static_cast<std::uint64_t>(streams_.size()));
    for (const auto& [name, stream] : streams_) {
        writer.string(name);
        writer.u64(stream.state);
        writer.u64(stream.increment);
        writer.u64(stream.draws);
    }
}

std::expected<DeterministicRandom, Diagnostic> DeterministicRandom::decode(BinaryReader& reader) {
    const auto version = reader.u32();
    if (version != algorithm_version) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "unsupported deterministic RNG version", {}});
    }
    DeterministicRandom result{reader.u64()};
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        auto name = reader.string();
        RandomStreamState stream{reader.u64(), reader.u64(), reader.u64()};
        if (name.empty() || (stream.increment & 1U) == 0U || result.streams_.contains(name)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid serialized random stream", {}});
        }
        result.streams_.emplace(std::move(name), stream);
    }
    if (!reader.ok()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          std::string{reader.error()}, {}});
    }
    return result;
}

} // namespace ludus
