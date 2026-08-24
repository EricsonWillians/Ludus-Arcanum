#include "toml.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ludus::studio::detail {
namespace {

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1U);
    }
    return value;
}

Diagnostic parse_error(std::string_view path, std::size_t line, std::size_t column,
                       std::string message) {
    return Diagnostic{DiagnosticCode::serialization_error, std::move(message),
                      SourceLocation{std::string{path}, line, column}};
}

std::string_view without_comment(std::string_view line) noexcept {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0U; index < line.size(); ++index) {
        const auto character = line[index];
        if (quoted && escaped) {
            escaped = false;
            continue;
        }
        if (quoted && character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            quoted = !quoted;
        } else if (character == '#' && !quoted) {
            return line.substr(0U, index);
        }
    }
    return line;
}

bool valid_key(std::string_view key) noexcept {
    return !key.empty() && std::ranges::all_of(key, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '_' || character == '-';
    });
}

std::expected<std::pair<std::string, std::size_t>, std::size_t>
parse_quoted(std::string_view value, std::size_t start) {
    if (start >= value.size() || value[start] != '"') {
        return std::unexpected(start);
    }
    std::string result;
    bool escaped = false;
    for (std::size_t index = start + 1U; index < value.size(); ++index) {
        const auto character = value[index];
        if (escaped) {
            switch (character) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                return std::unexpected(index);
            }
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return std::pair{std::move(result), index + 1U};
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            return std::unexpected(index);
        } else {
            result.push_back(character);
        }
    }
    return std::unexpected(value.size());
}

std::expected<TomlValue, std::size_t> parse_value(std::string_view value) {
    value = trim(value);
    if (value.empty()) {
        return std::unexpected(0U);
    }
    if (value.front() == '"') {
        auto parsed = parse_quoted(value, 0U);
        if (!parsed || !trim(value.substr(parsed->second)).empty()) {
            return std::unexpected(parsed ? parsed->second : parsed.error());
        }
        return TomlValue{std::move(parsed->first)};
    }
    if (value.front() == '[') {
        if (value.back() != ']') {
            return std::unexpected(value.size());
        }
        std::vector<std::string> result;
        auto body = value.substr(1U, value.size() - 2U);
        std::size_t cursor = 0U;
        while (cursor < body.size()) {
            while (cursor < body.size() &&
                   std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
                ++cursor;
            }
            if (cursor == body.size()) {
                break;
            }
            auto parsed = parse_quoted(body, cursor);
            if (!parsed) {
                return std::unexpected(parsed.error() + 1U);
            }
            result.push_back(std::move(parsed->first));
            cursor = parsed->second;
            while (cursor < body.size() &&
                   std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
                ++cursor;
            }
            if (cursor == body.size()) {
                break;
            }
            if (body[cursor] != ',') {
                return std::unexpected(cursor + 1U);
            }
            ++cursor;
        }
        return TomlValue{std::move(result)};
    }
    if (value == "true") {
        return TomlValue{true};
    }
    if (value == "false") {
        return TomlValue{false};
    }
    std::int64_t integer = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), integer);
    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
        return TomlValue{integer};
    }
    return std::unexpected(0U);
}

} // namespace

std::expected<std::vector<TomlTable>, Diagnostic>
parse_toml(std::string_view text, std::string_view path) {
    std::vector<TomlTable> tables;
    TomlTable* current = nullptr;
    std::size_t offset = 0U;
    std::size_t line_number = 1U;
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        const auto length = end == std::string_view::npos ? text.size() - offset : end - offset;
        auto line = trim(without_comment(text.substr(offset, length)));
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
            line = trim(line);
        }
        if (!line.empty()) {
            if (line.front() == '[') {
                const bool array = line.size() >= 4U && line.starts_with("[[") &&
                                   line.ends_with("]]");
                const auto opening = array ? 2U : 1U;
                const auto closing = array ? 2U : 1U;
                if ((!array && !line.ends_with(']')) || line.size() <= opening + closing) {
                    return std::unexpected(parse_error(path, line_number, 1U,
                                                       "invalid TOML table header"));
                }
                const auto name = trim(line.substr(opening, line.size() - opening - closing));
                if (!valid_key(name)) {
                    return std::unexpected(parse_error(path, line_number, opening + 1U,
                                                       "invalid TOML table name"));
                }
                if (!array && std::ranges::any_of(tables, [name](const TomlTable& table) {
                        return table.name == name && !table.array;
                    })) {
                    return std::unexpected(parse_error(path, line_number, 1U,
                                                       "duplicate TOML table: " +
                                                           std::string{name}));
                }
                tables.push_back(TomlTable{std::string{name}, array, line_number, {}});
                current = &tables.back();
            } else {
                if (current == nullptr) {
                    return std::unexpected(parse_error(
                        path, line_number, 1U, "TOML keys must belong to a table"));
                }
                const auto equals = line.find('=');
                if (equals == std::string_view::npos) {
                    return std::unexpected(parse_error(path, line_number, 1U,
                                                       "expected key = value"));
                }
                const auto key = trim(line.substr(0U, equals));
                if (!valid_key(key)) {
                    return std::unexpected(parse_error(path, line_number, 1U,
                                                       "invalid TOML key"));
                }
                if (std::ranges::any_of(current->entries, [key](const TomlEntry& entry) {
                        return entry.key == key;
                    })) {
                    return std::unexpected(parse_error(path, line_number, 1U,
                                                       "duplicate TOML key: " +
                                                           std::string{key}));
                }
                auto parsed = parse_value(line.substr(equals + 1U));
                if (!parsed) {
                    return std::unexpected(parse_error(
                        path, line_number, equals + parsed.error() + 2U,
                        "unsupported or malformed TOML value"));
                }
                current->entries.push_back(
                    TomlEntry{std::string{key}, std::move(*parsed), line_number});
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1U;
        ++line_number;
    }
    return tables;
}

std::string quote_toml(std::string_view value) {
    std::string result{"\""};
    for (const auto character : value) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string string_array_toml(const std::vector<std::string>& values) {
    std::string result{"["};
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        result += quote_toml(values[index]);
    }
    result.push_back(']');
    return result;
}

} // namespace ludus::studio::detail
