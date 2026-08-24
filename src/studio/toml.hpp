#pragma once

#include "ludus/core/diagnostic.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ludus::studio::detail {

using TomlValue = std::variant<std::string, std::int64_t, bool, std::vector<std::string>>;

struct TomlEntry {
    std::string key;
    TomlValue value;
    std::size_t line{0U};
};

struct TomlTable {
    std::string name;
    bool array{false};
    std::size_t line{0U};
    std::vector<TomlEntry> entries;
};

[[nodiscard]] std::expected<std::vector<TomlTable>, Diagnostic>
parse_toml(std::string_view text, std::string_view path);

[[nodiscard]] std::string quote_toml(std::string_view value);
[[nodiscard]] std::string string_array_toml(const std::vector<std::string>& values);

} // namespace ludus::studio::detail
