#include "ludus/core/binary.hpp"

#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>

namespace ludus {
namespace {

template <typename T>
void write_unsigned(std::vector<std::byte>& data, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        data.push_back(static_cast<std::byte>((value >> (index * 8U)) & static_cast<T>(0xffU)));
    }
}

template <typename T>
T read_unsigned(std::span<const std::byte> bytes) {
    static_assert(std::is_unsigned_v<T>);
    T result{0};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        result |= static_cast<T>(std::to_integer<unsigned int>(bytes[index])) << (index * 8U);
    }
    return result;
}

} // namespace

void BinaryWriter::u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }
void BinaryWriter::u32(std::uint32_t value) { write_unsigned(data_, value); }
void BinaryWriter::u64(std::uint64_t value) { write_unsigned(data_, value); }
void BinaryWriter::i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
void BinaryWriter::i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }
void BinaryWriter::boolean(bool value) { u8(value ? 1U : 0U); }

void BinaryWriter::string(std::string_view value) {
    u64(static_cast<std::uint64_t>(value.size()));
    for (const char character : value) {
        data_.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void BinaryWriter::bytes(std::span<const std::byte> value) {
    u64(static_cast<std::uint64_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
}

std::span<const std::byte> BinaryReader::consume(std::size_t count) {
    if (!ok()) {
        return {};
    }
    if (count > data_.size() - offset_) {
        error_ = "unexpected end of canonical data";
        return {};
    }
    const auto result = data_.subspan(offset_, count);
    offset_ += count;
    return result;
}

std::uint8_t BinaryReader::u8() {
    const auto value = consume(1);
    return value.empty() ? 0U : std::to_integer<std::uint8_t>(value.front());
}

std::uint32_t BinaryReader::u32() {
    const auto value = consume(sizeof(std::uint32_t));
    return value.empty() ? 0U : read_unsigned<std::uint32_t>(value);
}

std::uint64_t BinaryReader::u64() {
    const auto value = consume(sizeof(std::uint64_t));
    return value.empty() ? 0U : read_unsigned<std::uint64_t>(value);
}

std::int32_t BinaryReader::i32() { return std::bit_cast<std::int32_t>(u32()); }
std::int64_t BinaryReader::i64() { return std::bit_cast<std::int64_t>(u64()); }

bool BinaryReader::boolean() {
    const auto value = u8();
    if (value > 1U && ok()) {
        error_ = "invalid serialized boolean";
    }
    return value == 1U;
}

std::string BinaryReader::string() {
    const auto size = u64();
    if (!ok() || size > std::numeric_limits<std::size_t>::max()) {
        if (ok()) {
            error_ = "serialized string is too large";
        }
        return {};
    }
    const auto value = consume(static_cast<std::size_t>(size));
    std::string result;
    result.reserve(value.size());
    for (const auto byte : value) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return result;
}

std::vector<std::byte> BinaryReader::bytes() {
    const auto size = u64();
    if (!ok() || size > std::numeric_limits<std::size_t>::max()) {
        if (ok()) {
            error_ = "serialized byte sequence is too large";
        }
        return {};
    }
    const auto value = consume(static_cast<std::size_t>(size));
    return {value.begin(), value.end()};
}

void BinaryReader::invalidate(std::string message) {
    if (error_.empty()) {
        error_ = std::move(message);
    }
}

std::uint64_t canonical_hash(std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    auto hash = offset_basis;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= prime;
    }
    return hash;
}

std::string hash_hex(std::uint64_t hash) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

} // namespace ludus
