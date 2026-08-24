#include "ludus/studio/package_document.hpp"

#include "toml.hpp"

#include "ludus/core/symbol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace ludus::studio {
namespace {

using detail::TomlEntry;
using detail::TomlTable;

constexpr std::string_view default_python_source = R"python("""Rules for an editable Ludus Arcanum chess-like package."""

from ludus_arcanum import action, move

ORTHOGONAL = ("north", "east", "south", "west")
DIAGONAL = ("north_east", "south_east", "south_west", "north_west")
ADJACENT = ORTHOGONAL + DIAGONAL
KNIGHT = (
    "knight_nne", "knight_ene", "knight_ese", "knight_sse",
    "knight_ssw", "knight_wsw", "knight_wnw", "knight_nnw",
)

# Edit these immutable declarations, save, then choose Reload rules.
MOVEMENT_RULES = {
    "rook": move.rays(ORTHOGONAL).until_blocked().allow_empty().capture_enemy(),
    "bishop": move.rays(DIAGONAL).until_blocked().allow_empty().capture_enemy(),
    "queen": move.rays(ADJACENT).until_blocked().allow_empty().capture_enemy(),
    "king": move.jumps(ADJACENT).allow_empty().capture_enemy(),
    "knight": move.jumps(KNIGHT).allow_empty().capture_enemy(),
}

CAPTURE = 1 << 0
KING_CASTLE = 1 << 3
QUEEN_CASTLE = 1 << 4
PROMOTION = 1 << 5


@action("chess_move")
def chess_move(ctx, tx, actor, targets):
    flags = ctx.argument("move_flags")
    cursor = 2
    if flags & CAPTURE:
        tx.destroy(targets[cursor])
        cursor += 1
    tx.move(actor, targets[0])
    if flags & (KING_CASTLE | QUEEN_CASTLE):
        tx.move(targets[cursor], targets[cursor + 1])
    if flags & PROMOTION:
        tx.set_property(actor, "piece_type", ctx.argument("promotion"))
    metadata = targets[1]
    tx.set_property(metadata, "side_to_move", ctx.argument("next_side"))
    tx.set_property(metadata, "castling_rights", ctx.argument("next_castling"))
    tx.set_property(metadata, "en_passant", ctx.argument("next_en_passant"))
    tx.set_property(metadata, "halfmove_clock", ctx.argument("next_halfmove"))
    tx.set_property(metadata, "fullmove_number", ctx.argument("next_fullmove"))
)python";

Diagnostic document_error(const std::filesystem::path& path, std::string message,
                          std::size_t line = 0U) {
    return Diagnostic{DiagnosticCode::validation_failed, std::move(message),
                      SourceLocation{path.string(), line, 0U}};
}

bool valid_identifier(std::string_view value, bool allow_dot,
                      bool allow_hyphen = false) noexcept {
    if (value.empty()) {
        return false;
    }
    bool segment_start = true;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (allow_dot && character == '.') {
            if (segment_start) {
                return false;
            }
            segment_start = true;
            continue;
        }
        if (segment_start && std::isdigit(byte) != 0) {
            return false;
        }
        if (std::isalnum(byte) == 0 && character != '_' &&
            (!allow_hyphen || character != '-')) {
            return false;
        }
        segment_start = false;
    }
    return !segment_start;
}

bool valid_semantic_version(std::string_view value) noexcept {
    std::uint32_t components = 0U;
    bool has_digit = false;
    for (const auto character : value) {
        if (character >= '0' && character <= '9') {
            has_digit = true;
        } else if (character == '.' && has_digit && components < 2U) {
            ++components;
            has_digit = false;
        } else {
            return false;
        }
    }
    return components == 2U && has_digit;
}

bool safe_relative_path(const std::filesystem::path& path) noexcept {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    return std::ranges::none_of(path, [](const auto& component) {
        return component == ".." || component == "." || component.empty();
    });
}

std::filesystem::path module_source_path(std::string_view module) {
    std::string relative{module};
    std::ranges::replace(relative, '.', '/');
    return std::filesystem::path{relative + ".py"};
}

std::expected<std::string, Diagnostic> read_text(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(document_error(path, "unable to inspect package file: " +
                                                        error.message()));
    }
    if (size > PackageDocument::maximum_text_file_size) {
        return std::unexpected(document_error(path, "package text file exceeds the 1 MiB limit"));
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::unexpected(document_error(path, "unable to open package file"));
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    stream.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!stream && !result.empty()) {
        return std::unexpected(document_error(path, "unable to read complete package file"));
    }
    return result;
}

std::expected<void, Diagnostic> atomic_write(const std::filesystem::path& path,
                                             std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return std::unexpected(document_error(path, "unable to create package directory: " +
                                                        error.message()));
    }
    auto temporary = path;
    temporary += ".ludus-tmp";
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return std::unexpected(document_error(path, "unable to create temporary save file"));
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            std::filesystem::remove(temporary, error);
            return std::unexpected(document_error(path, "unable to write package file"));
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        const auto rename_error = error;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return std::unexpected(document_error(path, "unable to replace package file: " +
                                                        rename_error.message()));
    }
    return {};
}

std::expected<std::string, Diagnostic> read_binary(const std::filesystem::path& path,
                                                   std::uintmax_t maximum_size) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_size) {
        return std::unexpected(document_error(
            path, error ? "unable to inspect imported asset: " + error.message()
                        : "imported asset exceeds the 16 MiB compressed limit"));
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::unexpected(document_error(path, "unable to open imported asset"));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream && !bytes.empty()) {
        return std::unexpected(document_error(path, "unable to read complete imported asset"));
    }
    return bytes;
}

std::string safe_asset_stem(std::string stem) {
    for (auto& character : stem) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) == 0 && character != '-' && character != '_') {
            character = '-';
        }
    }
    while (!stem.empty() && (stem.front() == '-' || stem.front() == '_')) {
        stem.erase(stem.begin());
    }
    return stem.empty() ? "sprite" : stem;
}

std::optional<std::uint32_t> known_sprite_id(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 12U> names{
        "piece.ivory.pawn",   "piece.ivory.knight", "piece.ivory.bishop",
        "piece.ivory.rook",   "piece.ivory.queen",  "piece.ivory.king",
        "piece.iron.pawn",    "piece.iron.knight",  "piece.iron.bishop",
        "piece.iron.rook",    "piece.iron.queen",   "piece.iron.king"};
    const auto found = std::ranges::find(names, name);
    if (found != names.end()) {
        return static_cast<std::uint32_t>(std::distance(names.begin(), found));
    }
    constexpr std::string_view legacy_prefix = "legacy.unknown.sprite_";
    if (name.starts_with(legacy_prefix)) {
        std::uint32_t value = 0U;
        const auto suffix = name.substr(legacy_prefix.size());
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() &&
            value < 4'096U) {
            return value;
        }
    }
    return std::nullopt;
}

std::string known_sprite_name(std::uint32_t sprite, std::uint32_t owner,
                              std::string_view type) {
    constexpr std::array<std::string_view, 6U> types{
        "pawn", "knight", "bishop", "rook", "queen", "king"};
    const auto type_found = std::ranges::find(types, type);
    if (sprite < 6U && owner < 2U && type_found != types.end()) {
        return "piece." + std::string{owner == 0U ? "ivory." : "iron."} +
               std::string{*type_found};
    }
    constexpr std::array<std::string_view, 12U> names{
        "piece.ivory.pawn",   "piece.ivory.knight", "piece.ivory.bishop",
        "piece.ivory.rook",   "piece.ivory.queen",  "piece.ivory.king",
        "piece.iron.pawn",    "piece.iron.knight",  "piece.iron.bishop",
        "piece.iron.rook",    "piece.iron.queen",   "piece.iron.king"};
    if (sprite < names.size()) {
        return std::string{names[sprite]};
    }
    return "legacy.unknown.sprite_" + std::to_string(sprite);
}

const TomlTable* find_table(const std::vector<TomlTable>& tables, std::string_view name,
                            bool array = false) noexcept {
    const auto found = std::ranges::find_if(tables, [name, array](const TomlTable& table) {
        return table.name == name && table.array == array;
    });
    return found == tables.end() ? nullptr : &*found;
}

const TomlEntry* find_entry(const TomlTable& table, std::string_view key) noexcept {
    const auto found = std::ranges::find(table.entries, key, &TomlEntry::key);
    return found == table.entries.end() ? nullptr : &*found;
}

template <typename Value>
std::expected<Value, Diagnostic> required_value(const TomlTable& table,
                                                std::string_view key,
                                                const std::filesystem::path& path) {
    const auto* entry = find_entry(table, key);
    if (entry == nullptr) {
        return std::unexpected(document_error(path, "missing required key: " +
                                                        std::string{key},
                                              table.line));
    }
    const auto* value = std::get_if<Value>(&entry->value);
    if (value == nullptr) {
        return std::unexpected(document_error(path, "invalid type for key: " +
                                                        std::string{key},
                                              entry->line));
    }
    return *value;
}

std::expected<std::uint32_t, Diagnostic>
required_u32(const TomlTable& table, std::string_view key,
             const std::filesystem::path& path) {
    auto value = required_value<std::int64_t>(table, key, path);
    if (!value || *value < 0 ||
        *value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        if (!value) {
            return std::unexpected(value.error());
        }
        const auto* entry = find_entry(table, key);
        return std::unexpected(document_error(path, "integer is out of range: " +
                                                        std::string{key},
                                              entry == nullptr ? table.line : entry->line));
    }
    return static_cast<std::uint32_t>(*value);
}

std::expected<void, Diagnostic>
reject_unknown(const TomlTable& table, std::initializer_list<std::string_view> allowed,
               const std::filesystem::path& path) {
    for (const auto& entry : table.entries) {
        if (std::ranges::find(allowed, entry.key) == allowed.end()) {
            return std::unexpected(document_error(path, "unknown key in [" + table.name +
                                                            "]: " + entry.key,
                                                  entry.line));
        }
    }
    return {};
}

std::expected<PackageManifest, Diagnostic>
parse_manifest(std::string_view text, const std::filesystem::path& path) {
    auto tables = detail::parse_toml(text, path.string());
    if (!tables) {
        return std::unexpected(tables.error());
    }
    if (std::ranges::any_of(*tables, [](const TomlTable& table) {
            return (table.name != "package" && table.name != "native") || table.array;
        })) {
        return std::unexpected(document_error(path, "manifest contains an unsupported table"));
    }
    const auto* package = find_table(*tables, "package");
    const auto* native = find_table(*tables, "native");
    if (package == nullptr || native == nullptr) {
        return std::unexpected(document_error(path, "manifest requires [package] and [native]"));
    }
    if (auto known = reject_unknown(
            *package,
            {"id", "version", "engine_api", "entry_point", "board", "save_compatibility",
             "visuals", "assets", "permissions", "dependencies"},
            path);
        !known) {
        return std::unexpected(known.error());
    }
    if (auto known = reject_unknown(*native, {"extensions", "enabled_by_default"}, path);
        !known) {
        return std::unexpected(known.error());
    }

    auto id = required_value<std::string>(*package, "id", path);
    auto version = required_value<std::string>(*package, "version", path);
    auto engine_api = required_value<std::string>(*package, "engine_api", path);
    auto entry_point = required_value<std::string>(*package, "entry_point", path);
    auto board = required_value<std::string>(*package, "board", path);
    std::optional<std::string> visuals;
    if (const auto* entry = find_entry(*package, "visuals"); entry != nullptr) {
        const auto* value = std::get_if<std::string>(&entry->value);
        if (value == nullptr) {
            return std::unexpected(
                document_error(path, "invalid type for key: visuals", entry->line));
        }
        visuals = *value;
    }
    auto save_compatibility = required_u32(*package, "save_compatibility", path);
    auto assets = required_value<std::vector<std::string>>(*package, "assets", path);
    auto permissions =
        required_value<std::vector<std::string>>(*package, "permissions", path);
    auto dependencies =
        required_value<std::vector<std::string>>(*package, "dependencies", path);
    auto extensions = required_value<std::vector<std::string>>(*native, "extensions", path);
    auto enabled = required_value<bool>(*native, "enabled_by_default", path);
    if (!id || !version || !engine_api || !entry_point || !board || !save_compatibility ||
        !assets || !permissions || !dependencies || !extensions || !enabled) {
        for (const auto* candidate : {id ? nullptr : &id.error(),
                                     version ? nullptr : &version.error(),
                                     engine_api ? nullptr : &engine_api.error(),
                                     entry_point ? nullptr : &entry_point.error(),
                                     board ? nullptr : &board.error(),
                                     save_compatibility ? nullptr : &save_compatibility.error(),
                                     assets ? nullptr : &assets.error(),
                                     permissions ? nullptr : &permissions.error(),
                                     dependencies ? nullptr : &dependencies.error(),
                                     extensions ? nullptr : &extensions.error(),
                                     enabled ? nullptr : &enabled.error()}) {
            if (candidate != nullptr) {
                return std::unexpected(*candidate);
            }
        }
    }
    const auto semantic_error = [&](std::string_view key, std::string message) {
        const auto* entry = find_entry(*package, key);
        return document_error(path, std::move(message),
                              entry == nullptr ? package->line : entry->line);
    };
    if (!valid_identifier(*id, true, true)) {
        return std::unexpected(semantic_error("id", "invalid package identifier"));
    }
    if (!valid_semantic_version(*version)) {
        return std::unexpected(semantic_error("version", "invalid semantic version"));
    }
    if (*engine_api != ">=0.1.0,<0.2.0" && *engine_api != "0.1.0") {
        return std::unexpected(
            semantic_error("engine_api", "package requires an unsupported engine API"));
    }
    if (!valid_identifier(*entry_point, true)) {
        return std::unexpected(semantic_error("entry_point", "invalid Python entry point"));
    }
    if (!safe_relative_path(*board)) {
        return std::unexpected(semantic_error("board", "board path escapes the package"));
    }
    if (visuals && !safe_relative_path(*visuals)) {
        return std::unexpected(semantic_error("visuals", "visuals path escapes the package"));
    }
    if (*save_compatibility == 0U) {
        return std::unexpected(
            semantic_error("save_compatibility", "save compatibility must be positive"));
    }
    if (*enabled) {
        const auto* entry = find_entry(*native, "enabled_by_default");
        return std::unexpected(document_error(
            path, "native extensions must be disabled by default",
            entry == nullptr ? native->line : entry->line));
    }
    return PackageManifest{std::move(*id),
                           std::move(*version),
                           std::move(*engine_api),
                           std::move(*entry_point),
                           std::move(*board),
                           std::move(visuals),
                           *save_compatibility,
                           std::move(*assets),
                           std::move(*permissions),
                           std::move(*dependencies),
                           std::move(*extensions),
                           *enabled};
}

std::expected<BoardDefinition, Diagnostic>
parse_board(std::string_view text, const std::filesystem::path& path) {
    auto tables = detail::parse_toml(text, path.string());
    if (!tables) {
        return std::unexpected(tables.error());
    }
    if (std::ranges::any_of(*tables, [](const TomlTable& table) {
            return table.name != "board" && table.name != "entity";
        })) {
        return std::unexpected(document_error(path, "board file contains an unsupported table"));
    }
    const auto* board_table = find_table(*tables, "board");
    if (board_table == nullptr) {
        return std::unexpected(document_error(path, "board file requires a [board] table"));
    }
    if (auto known = reject_unknown(
            *board_table, {"kind", "width", "height", "side_to_move", "castling_rights"},
            path);
        !known) {
        return std::unexpected(known.error());
    }
    auto kind = required_value<std::string>(*board_table, "kind", path);
    auto width = required_u32(*board_table, "width", path);
    auto height = required_u32(*board_table, "height", path);
    auto side = required_u32(*board_table, "side_to_move", path);
    auto rights = required_u32(*board_table, "castling_rights", path);
    if (!kind || !width || !height || !side || !rights) {
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (!width) {
            return std::unexpected(width.error());
        }
        if (!height) {
            return std::unexpected(height.error());
        }
        if (!side) {
            return std::unexpected(side.error());
        }
        return std::unexpected(rights.error());
    }
    if (*kind != "rectangular") {
        return std::unexpected(document_error(path, "only rectangular boards are supported yet",
                                              board_table->line));
    }
    if (*width == 0U || *height == 0U || *width > BoardDefinition::maximum_extent ||
        *height > BoardDefinition::maximum_extent ||
        static_cast<std::uint64_t>(*width) * *height > 65'536U || *side > 1U ||
        *rights > 15U) {
        return std::unexpected(document_error(path, "board metadata is out of range",
                                              board_table->line));
    }
    BoardDefinition result{*width, *height, *side, *rights, {}};
    std::set<std::string> names;
    std::set<std::pair<std::uint32_t, std::uint32_t>> locations;
    for (const auto& table : *tables) {
        if (table.name != "entity") {
            continue;
        }
        if (!table.array) {
            return std::unexpected(document_error(path, "entities must use [[entity]] tables",
                                                  table.line));
        }
        if (auto known = reject_unknown(table, {"name", "type", "owner", "x", "y", "sprite",
                                                     "sprite_name"},
                                        path);
            !known) {
            return std::unexpected(known.error());
        }
        auto name = required_value<std::string>(table, "name", path);
        auto type = required_value<std::string>(table, "type", path);
        auto owner = required_u32(table, "owner", path);
        auto x = required_u32(table, "x", path);
        auto y = required_u32(table, "y", path);
        std::optional<std::uint32_t> sprite;
        std::string sprite_name;
        if (find_entry(table, "sprite") != nullptr) {
            auto numeric = required_u32(table, "sprite", path);
            if (!numeric) {
                return std::unexpected(numeric.error());
            }
            sprite = *numeric;
        }
        if (const auto* entry = find_entry(table, "sprite_name"); entry != nullptr) {
            const auto* named = std::get_if<std::string>(&entry->value);
            if (named == nullptr || !valid_identifier(*named, true)) {
                return std::unexpected(document_error(path, "invalid named sprite", entry->line));
            }
            sprite_name = *named;
            if (const auto known = known_sprite_id(sprite_name)) {
                sprite = *known;
            } else if (!sprite) {
                sprite = 4'095U;
            }
        }
        if (!name || !type || !owner || !x || !y || !sprite) {
            if (!name) {
                return std::unexpected(name.error());
            }
            if (!type) {
                return std::unexpected(type.error());
            }
            if (!owner) {
                return std::unexpected(owner.error());
            }
            if (!x) {
                return std::unexpected(x.error());
            }
            if (!y) {
                return std::unexpected(y.error());
            }
            return std::unexpected(document_error(path, "entity requires sprite_name or sprite",
                                                  table.line));
        }
        if (!valid_identifier(*name, false) || !valid_identifier(*type, false) ||
            *x >= result.width || *y >= result.height || *owner > 63U || *sprite >= 4'096U ||
            !names.insert(*name).second || !locations.emplace(*x, *y).second) {
            return std::unexpected(document_error(path, "invalid or duplicate board entity",
                                                  table.line));
        }
        if (sprite_name.empty()) {
            sprite_name = known_sprite_name(*sprite, *owner, *type);
            if (const auto mapped = known_sprite_id(sprite_name)) {
                sprite = *mapped;
            }
        }
        result.entities.push_back(BoardEntity{std::move(*name), std::move(*type), *owner,
                                              *x, *y, *sprite, std::move(sprite_name)});
    }
    return result;
}

std::string serialize_manifest(const PackageManifest& manifest) {
    std::ostringstream output;
    output << "[package]\n"
           << "id = " << detail::quote_toml(manifest.id) << '\n'
           << "version = " << detail::quote_toml(manifest.version) << '\n'
           << "engine_api = " << detail::quote_toml(manifest.engine_api) << '\n'
           << "entry_point = " << detail::quote_toml(manifest.entry_point) << '\n'
           << "board = " << detail::quote_toml(manifest.board_file) << '\n'
           << (manifest.visuals ? "visuals = " + detail::quote_toml(*manifest.visuals) + "\n"
                                : std::string{})
           << "save_compatibility = " << manifest.save_compatibility << '\n'
           << "assets = " << detail::string_array_toml(manifest.assets) << '\n'
           << "permissions = " << detail::string_array_toml(manifest.permissions) << '\n'
           << "dependencies = " << detail::string_array_toml(manifest.dependencies) << "\n\n"
           << "[native]\n"
           << "extensions = " << detail::string_array_toml(manifest.native_extensions) << '\n'
           << "enabled_by_default = "
           << (manifest.native_enabled_by_default ? "true" : "false") << '\n';
    return output.str();
}

std::string serialize_board(const BoardDefinition& board) {
    std::ostringstream output;
    output << "[board]\n"
           << "kind = \"rectangular\"\n"
           << "width = " << board.width << '\n'
           << "height = " << board.height << '\n'
           << "side_to_move = " << board.side_to_move << '\n'
           << "castling_rights = " << board.castling_rights << '\n';
    for (const auto& entity : board.entities) {
        output << "\n[[entity]]\n"
               << "name = " << detail::quote_toml(entity.name) << '\n'
               << "type = " << detail::quote_toml(entity.type) << '\n'
               << "owner = " << entity.owner << '\n'
               << "x = " << entity.x << '\n'
               << "y = " << entity.y << '\n'
               << "sprite_name = "
               << detail::quote_toml(entity.sprite_name.empty()
                                         ? known_sprite_name(entity.sprite, entity.owner,
                                                             entity.type)
                                         : entity.sprite_name)
               << '\n';
    }
    return output.str();
}

std::optional<std::uint32_t> sprite_for(std::string_view type) noexcept {
    constexpr std::array names{"pawn", "knight", "bishop", "rook", "queen", "king"};
    const auto found = std::ranges::find(names, type);
    if (found == names.end()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::distance(names.begin(), found));
}

std::vector<BoardEntity> initial_chess_entities() {
    constexpr std::array back_rank{"rook", "knight", "bishop", "queen",
                                   "king", "bishop", "knight", "rook"};
    std::vector<BoardEntity> result;
    result.reserve(32U);
    for (std::uint32_t owner = 0U; owner < 2U; ++owner) {
        const auto back_y = owner == 0U ? 0U : 7U;
        const auto pawn_y = owner == 0U ? 1U : 6U;
        const auto color = owner == 0U ? "white" : "black";
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            const auto file = static_cast<char>('a' + x);
            result.push_back(BoardEntity{
                std::string{color} + "_" + back_rank[x] + "_" + file,
                back_rank[x], owner, x, back_y, *sprite_for(back_rank[x]) + owner * 6U,
                known_sprite_name(*sprite_for(back_rank[x]), owner, back_rank[x])});
            result.push_back(BoardEntity{std::string{color} + "_pawn_" + file, "pawn", owner,
                                         x, pawn_y, *sprite_for("pawn") + owner * 6U,
                                         known_sprite_name(*sprite_for("pawn"), owner, "pawn")});
        }
    }
    return result;
}

Vec2 space_center(std::uint32_t index, std::uint32_t width,
                  std::uint32_t height) noexcept {
    return {static_cast<float>(index % width) - (static_cast<float>(width) - 1.0F) * 0.5F,
            static_cast<float>(index / width) - (static_cast<float>(height) - 1.0F) * 0.5F};
}

} // namespace

std::expected<PackageDocument, Diagnostic>
PackageDocument::create(const std::filesystem::path& root, std::string package_id,
                        std::uint32_t width, std::uint32_t height) {
    const auto normalized = std::filesystem::absolute(root).lexically_normal();
    std::error_code error;
    const auto exists = std::filesystem::exists(normalized / "game.toml", error);
    if (error) {
        return std::unexpected(document_error(normalized / "game.toml",
                                              "unable to inspect package destination: " +
                                                  error.message()));
    }
    if (exists) {
        return std::unexpected(document_error(normalized / "game.toml",
                                              "refusing to replace an existing package"));
    }
    PackageManifest manifest;
    manifest.id = std::move(package_id);
    BoardDefinition board;
    board.width = width;
    board.height = height;
    board.castling_rights = width == 8U && height == 8U ? 15U : 0U;
    if (width == 8U && height == 8U) {
        board.entities = initial_chess_entities();
    }
    PackageDocument result{normalized, std::move(manifest), std::move(board),
                           std::string{default_python_source}};
    if (const auto diagnostics = result.validate(); !diagnostics.empty()) {
        return std::unexpected(diagnostics.front());
    }
    if (auto saved = result.save(); !saved) {
        return std::unexpected(saved.error());
    }
    return result;
}

std::expected<PackageDocument, Diagnostic>
PackageDocument::open(const std::filesystem::path& root) {
    const auto normalized = std::filesystem::absolute(root).lexically_normal();
    const auto manifest_path = normalized / "game.toml";
    auto manifest_text = read_text(manifest_path);
    if (!manifest_text) {
        return std::unexpected(manifest_text.error());
    }
    auto manifest = parse_manifest(*manifest_text, manifest_path);
    if (!manifest) {
        return std::unexpected(manifest.error());
    }
    if (!safe_relative_path(manifest->board_file) ||
        !valid_identifier(manifest->entry_point, true)) {
        return std::unexpected(document_error(manifest_path,
                                              "manifest contains an unsafe board or module path"));
    }
    const auto board_path = normalized / manifest->board_file;
    auto board_text = read_text(board_path);
    if (!board_text) {
        return std::unexpected(board_text.error());
    }
    auto board = parse_board(*board_text, board_path);
    if (!board) {
        return std::unexpected(board.error());
    }
    const auto source_path = normalized / module_source_path(manifest->entry_point);
    auto source = read_text(source_path);
    if (!source) {
        return std::unexpected(source.error());
    }
    PackageDocument result{normalized, std::move(*manifest), std::move(*board),
                           std::move(*source)};
    if (const auto diagnostics = result.validate(); !diagnostics.empty()) {
        return std::unexpected(diagnostics.front());
    }
    return result;
}

std::expected<void, Diagnostic> PackageDocument::save() const {
    if (const auto diagnostics = validate(); !diagnostics.empty()) {
        return std::unexpected(diagnostics.front());
    }
    if (auto written = atomic_write(manifest_path(), serialize_manifest(manifest_)); !written) {
        return written;
    }
    if (auto written = atomic_write(board_path(), serialize_board(board_)); !written) {
        return written;
    }
    const auto init_path = python_path().parent_path() / "__init__.py";
    std::error_code error;
    if (!std::filesystem::exists(init_path, error)) {
        if (auto written = atomic_write(init_path, "\"\"\"Ludus Arcanum game package.\"\"\"\n");
            !written) {
            return written;
        }
    }
    return atomic_write(python_path(), python_source_);
}

std::vector<Diagnostic> PackageDocument::validate() const {
    std::vector<Diagnostic> result;
    const auto add = [&result, this](std::string message, const std::filesystem::path& path) {
        result.push_back(document_error(root_ / path, std::move(message)));
    };
    if (!valid_identifier(manifest_.id, true, true)) {
        add("package id must contain identifier segments", "game.toml");
    }
    if (!valid_semantic_version(manifest_.version)) {
        add("package version must be major.minor.patch", "game.toml");
    }
    if (manifest_.engine_api.empty()) {
        add("engine API requirement cannot be empty", "game.toml");
    } else if (manifest_.engine_api != ">=0.1.0,<0.2.0" &&
               manifest_.engine_api != "0.1.0") {
        add("package requires an unsupported engine API", "game.toml");
    }
    if (!valid_identifier(manifest_.entry_point, true)) {
        add("entry point must be a dotted Python module", "game.toml");
    }
    if (!safe_relative_path(manifest_.board_file)) {
        add("board path must stay inside the package", "game.toml");
    }
    if (manifest_.visuals && !safe_relative_path(*manifest_.visuals)) {
        add("visuals path must stay inside the package", "game.toml");
    }
    if (manifest_.save_compatibility == 0U) {
        add("save compatibility version must be positive", "game.toml");
    }
    if (manifest_.native_enabled_by_default) {
        add("native extensions must be disabled by default", "game.toml");
    }
    for (const auto& asset : manifest_.assets) {
        if (!safe_relative_path(asset)) {
            add("asset paths must stay inside the package", "game.toml");
        }
    }
    for (const auto& dependency : manifest_.dependencies) {
        if (!valid_identifier(dependency, true, true)) {
            add("dependency identifiers are invalid", "game.toml");
        }
    }
    for (const auto& extension : manifest_.native_extensions) {
        if (!safe_relative_path(extension)) {
            add("native extension paths must stay inside the package", "game.toml");
        }
    }
    if (board_.width == 0U || board_.height == 0U ||
        board_.width > BoardDefinition::maximum_extent ||
        board_.height > BoardDefinition::maximum_extent ||
        static_cast<std::uint64_t>(board_.width) * board_.height > 65'536U) {
        add("board dimensions must describe between 1 and 65,536 spaces", manifest_.board_file);
    }
    if (board_.side_to_move > 1U || board_.castling_rights > 15U) {
        add("chess-like board metadata is out of range", manifest_.board_file);
    }
    if (board_.entities.size() > BoardDefinition::maximum_entities) {
        add("board entity count exceeds the package limit", manifest_.board_file);
    }
    std::set<std::string> names;
    std::set<std::pair<std::uint32_t, std::uint32_t>> locations;
    for (const auto& entity : board_.entities) {
        if (!valid_identifier(entity.name, false) || !valid_identifier(entity.type, false)) {
            add("entity names and types must be identifiers", manifest_.board_file);
        }
        if (!names.insert(entity.name).second) {
            add("duplicate entity name: " + entity.name, manifest_.board_file);
        }
        if (entity.x >= board_.width || entity.y >= board_.height) {
            add("entity is outside the generated board: " + entity.name, manifest_.board_file);
        }
        if (!locations.emplace(entity.x, entity.y).second) {
            add("multiple chess-like entities occupy one space", manifest_.board_file);
        }
        if (entity.owner > 63U || entity.sprite >= 4'096U) {
            add("entity owner or sprite is out of range: " + entity.name,
                manifest_.board_file);
        }
        if (!entity.sprite_name.empty() && !valid_identifier(entity.sprite_name, true)) {
            add("entity named sprite is invalid: " + entity.name, manifest_.board_file);
        }
    }
    if (python_source_.empty() || python_source_.size() > maximum_text_file_size) {
        add("Python entry point must contain at most 1 MiB of source",
            module_source_path(manifest_.entry_point));
    }
    return result;
}

std::expected<Topology, Diagnostic> PackageDocument::topology() const {
    SymbolTable<DirectionId> directions;
    const RectangularDirections ids{directions.intern("north"), directions.intern("east"),
                                    directions.intern("south"), directions.intern("west")};
    return make_rectangular_grid(board_.width, board_.height, ids);
}

std::expected<RenderSnapshot, Diagnostic>
PackageDocument::preview_snapshot(std::uint64_t revision) const {
    if (const auto diagnostics = validate(); !diagnostics.empty()) {
        return std::unexpected(diagnostics.front());
    }
    auto graph = topology();
    if (!graph) {
        return std::unexpected(graph.error());
    }
    RenderSnapshot result;
    result.revision = revision;
    result.static_revision = revision;
    result.dynamic_revision = revision;
    const auto half_width = static_cast<float>(board_.width) * 0.5F;
    const auto half_height = static_cast<float>(board_.height) * 0.5F;
    result.world_bounds = {{-half_width - 0.25F, -half_height - 0.25F},
                           {half_width + 0.25F, half_height + 0.25F}};
    result.spaces.reserve(graph->spaces().size());
    for (const auto& space : graph->spaces()) {
        const auto center = space_center(space.id.index(), board_.width, board_.height);
        const auto rank = space.id.index() / board_.width;
        const auto file = space.id.index() % board_.width;
        const auto color = (rank + file) % 2U == 0U
                               ? Color{0.18F, 0.22F, 0.28F, 1.0F}
                               : Color{0.28F, 0.34F, 0.42F, 1.0F};
        result.spaces.push_back(
            SpaceVisual{space.id,
                        {{center.x - 0.43F, center.y - 0.43F},
                         {center.x + 0.43F, center.y + 0.43F}},
                        color});
    }
    for (const auto& link : graph->links()) {
        if (link.from.index() >= link.to.index()) {
            continue;
        }
        result.links.push_back(LinkVisual{
            link.from, link.to, space_center(link.from.index(), board_.width, board_.height),
            space_center(link.to.index(), board_.width, board_.height),
            Color{0.2F, 0.68F, 0.95F, 0.72F}, 0.055F});
    }
    std::optional<VisualTheme> theme;
    if (manifest_.visuals) {
        auto loaded_theme = visual_theme();
        if (!loaded_theme) {
            return std::unexpected(loaded_theme.error());
        }
        theme = std::move(*loaded_theme);
    }
    result.pieces.reserve(board_.entities.size());
    for (std::size_t index = 0U; index < board_.entities.size(); ++index) {
        const auto& entity = board_.entities[index];
        const auto location = rectangular_space_id(entity.x, entity.y, board_.width);
        const auto center = space_center(location.index(), board_.width, board_.height);
        const auto sprite = theme && !entity.sprite_name.empty()
                                ? theme->sprite(entity.sprite_name)
                                      .value_or(SpriteId{4'095U})
                                : SpriteId{entity.sprite};
        result.pieces.push_back(PieceVisual{
            EntityId{static_cast<std::uint32_t>(index), 1U}, location, center, {0.76F, 0.76F},
            sprite, Color{1.0F, 1.0F, 1.0F, 1.0F},
            1.0F});
    }
    result.status = "Edit preview — " + std::to_string(board_.width) + "x" +
                    std::to_string(board_.height) + " — " +
                    std::to_string(graph->links().size()) + " directed links — " +
                    std::to_string(board_.entities.size()) + " entities";
    return result;
}

std::expected<std::filesystem::path, Diagnostic>
PackageDocument::import_png(const std::filesystem::path& source, std::string sprite_key) {
    if (!valid_identifier(sprite_key, true)) {
        return std::unexpected(document_error(source,
                                              "sprite key must be a dotted identifier"));
    }
    auto decoded = load_png_rgba(source);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto bytes = read_binary(source, 16U * 1024U * 1024U);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }

    const auto assets_directory = root_ / "assets";
    const auto stem = safe_asset_stem(source.stem().string());
    auto relative = std::filesystem::path{"assets"} / (stem + ".png");
    std::error_code error;
    for (std::uint32_t suffix = 2U; std::filesystem::exists(root_ / relative, error); ++suffix) {
        if (error || suffix > 10'000U) {
            return std::unexpected(document_error(
                assets_directory, "unable to choose a collision-safe asset filename"));
        }
        relative = std::filesystem::path{"assets"} /
                   (stem + "-" + std::to_string(suffix) + ".png");
    }

    const auto previous_manifest = manifest_;
    if (!manifest_.visuals) {
        manifest_.visuals = "visuals/theme.toml";
    }
    const auto theme_path = root_ / *manifest_.visuals;
    std::optional<std::string> previous_theme;
    if (std::filesystem::exists(theme_path, error)) {
        auto existing_theme = read_text(theme_path);
        if (!existing_theme) {
            manifest_ = previous_manifest;
            return std::unexpected(existing_theme.error());
        }
        previous_theme = std::move(*existing_theme);
    }
    std::string candidate = previous_theme.value_or(
        "[theme]\n"
        "id = \"studio-theme\"\n"
        "display_name = \"Studio Theme\"\n"
        "font_family = \"Serif\"\n"
        "background = \"#090b11ff\"\n");
    candidate += "\n[[sprite]]\n"
                 "id = " + detail::quote_toml(sprite_key) + "\n"
                 "source = " + detail::quote_toml(relative.generic_string()) + "\n"
                 "pivot_x = 0.5\n"
                 "pivot_y = 0.5\n"
                 "world_width = 1.0\n"
                 "world_height = 1.0\n"
                 "filter = \"linear\"\n";
    manifest_.assets.push_back(relative.generic_string());
    std::ranges::sort(manifest_.assets);
    manifest_.assets.erase(std::ranges::unique(manifest_.assets).begin(),
                           manifest_.assets.end());

    const auto destination = root_ / relative;
    const auto rollback = [&] {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        if (previous_theme) {
            static_cast<void>(atomic_write(theme_path, *previous_theme));
        } else {
            std::filesystem::remove(theme_path, ignored);
        }
        manifest_ = previous_manifest;
    };
    if (auto written = atomic_write(destination, *bytes); !written) {
        manifest_ = previous_manifest;
        return std::unexpected(written.error());
    }
    if (auto written = atomic_write(theme_path, candidate); !written) {
        rollback();
        return std::unexpected(written.error());
    }
    auto validated = VisualTheme::load(root_, *manifest_.visuals, manifest_.assets);
    if (!validated) {
        const auto diagnostic = validated.error();
        rollback();
        return std::unexpected(diagnostic);
    }
    if (auto saved = save(); !saved) {
        const auto diagnostic = saved.error();
        rollback();
        return std::unexpected(diagnostic);
    }
    return relative;
}

std::expected<VisualTheme, Diagnostic> PackageDocument::visual_theme() const {
    if (!manifest_.visuals) {
        return std::unexpected(document_error(manifest_path(),
                                              "package does not declare a visual theme"));
    }
    return VisualTheme::load(root_, *manifest_.visuals, manifest_.assets);
}

std::expected<void, Diagnostic>
PackageDocument::set_manifest(PackageManifest manifest) {
    auto previous = std::move(manifest_);
    manifest_ = std::move(manifest);
    const auto diagnostics = validate();
    if (!diagnostics.empty()) {
        manifest_ = std::move(previous);
        return std::unexpected(diagnostics.front());
    }
    return {};
}

std::expected<void, Diagnostic>
PackageDocument::regenerate_board(std::uint32_t width, std::uint32_t height) {
    const auto previous = board_;
    board_.width = width;
    board_.height = height;
    std::erase_if(board_.entities, [width, height](const BoardEntity& entity) {
        return entity.x >= width || entity.y >= height;
    });
    if (width != 8U || height != 8U) {
        board_.castling_rights = 0U;
    }
    const auto diagnostics = validate();
    if (!diagnostics.empty()) {
        board_ = previous;
        return std::unexpected(diagnostics.front());
    }
    return {};
}

std::expected<void, Diagnostic> PackageDocument::reset_chess_setup() {
    if (board_.width != 8U || board_.height != 8U) {
        return std::unexpected(document_error(board_path(),
                                              "the chess template requires an 8x8 board"));
    }
    board_.entities = initial_chess_entities();
    board_.side_to_move = 0U;
    board_.castling_rights = 15U;
    return {};
}

std::expected<void, Diagnostic>
PackageDocument::upsert_entity(BoardEntity entity, std::string_view replaced_name) {
    auto previous = board_.entities;
    if (!replaced_name.empty() && replaced_name != entity.name) {
        std::erase_if(board_.entities, [replaced_name](const BoardEntity& candidate) {
            return candidate.name == replaced_name;
        });
    }
    const auto found = std::ranges::find(board_.entities, entity.name, &BoardEntity::name);
    if (found == board_.entities.end()) {
        board_.entities.push_back(std::move(entity));
    } else {
        *found = std::move(entity);
    }
    const auto diagnostics = validate();
    if (!diagnostics.empty()) {
        board_.entities = std::move(previous);
        return std::unexpected(diagnostics.front());
    }
    std::ranges::sort(board_.entities, [](const BoardEntity& left, const BoardEntity& right) {
        if (left.y != right.y) {
            return left.y < right.y;
        }
        if (left.x != right.x) {
            return left.x < right.x;
        }
        return left.name < right.name;
    });
    return {};
}

bool PackageDocument::remove_entity(std::string_view name) {
    return std::erase_if(board_.entities,
                         [name](const BoardEntity& entity) { return entity.name == name; }) != 0U;
}

std::filesystem::path PackageDocument::manifest_path() const { return root_ / "game.toml"; }

std::filesystem::path PackageDocument::board_path() const {
    return root_ / manifest_.board_file;
}

std::filesystem::path PackageDocument::python_path() const {
    return root_ / module_source_path(manifest_.entry_point);
}

const BoardEntity* PackageDocument::entity_at(std::uint32_t x,
                                               std::uint32_t y) const noexcept {
    const auto found = std::ranges::find_if(board_.entities, [x, y](const BoardEntity& entity) {
        return entity.x == x && entity.y == y;
    });
    return found == board_.entities.end() ? nullptr : &*found;
}

std::string format_diagnostic(const Diagnostic& diagnostic) {
    std::string result;
    if (!diagnostic.source.path.empty()) {
        result += diagnostic.source.path;
        if (diagnostic.source.line != 0U) {
            result += ':' + std::to_string(diagnostic.source.line);
            if (diagnostic.source.column != 0U) {
                result += ':' + std::to_string(diagnostic.source.column);
            }
        }
        result += ": ";
    }
    result += diagnostic.message;
    if (!diagnostic.detail.empty()) {
        result += "\n" + diagnostic.detail;
    }
    return result;
}

} // namespace ludus::studio
