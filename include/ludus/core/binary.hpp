#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ludus {

class BinaryWriter {
  public:
    void u8(std::uint8_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i32(std::int32_t value);
    void i64(std::int64_t value);
    void boolean(bool value);
    void string(std::string_view value);
    void bytes(std::span<const std::byte> value);

    [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::byte> take() && noexcept { return std::move(data_); }

  private:
    std::vector<std::byte> data_;
};

class BinaryReader {
  public:
    explicit BinaryReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] std::int32_t i32();
    [[nodiscard]] std::int64_t i64();
    [[nodiscard]] bool boolean();
    [[nodiscard]] std::string string();
    [[nodiscard]] std::vector<std::byte> bytes();

    [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }
    void invalidate(std::string message);

  private:
    [[nodiscard]] std::span<const std::byte> consume(std::size_t count);

    std::span<const std::byte> data_;
    std::size_t offset_{0};
    std::string error_;
};

[[nodiscard]] std::uint64_t canonical_hash(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::string hash_hex(std::uint64_t hash);

} // namespace ludus
