#include "ludus/render/theme.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace ludus {
namespace {

struct ParsedSprite {
    std::string id;
    std::string source;
    Vec2 pivot{0.5F, 0.5F};
    Vec2 size{1.0F, 1.0F};
    TextureFilter filter{TextureFilter::linear};
    std::optional<std::uint32_t> region_x;
    std::optional<std::uint32_t> region_y;
    std::optional<std::uint32_t> region_width;
    std::optional<std::uint32_t> region_height;
    std::size_t line{0U};
};

struct ParsedAnimation {
    std::string id;
    std::vector<std::string> frame_ids;
    std::uint32_t frame_milliseconds{100U};
    bool loop{true};
    std::size_t line{0U};
};

struct ParsedColor {
    std::string id;
    Color value;
    bool has_value{false};
    std::size_t line{0U};
};

Diagnostic theme_error(const std::filesystem::path& path, std::size_t line,
                       std::string message) {
    return Diagnostic{DiagnosticCode::serialization_error, std::move(message),
                      SourceLocation{path.string(), line, 1U}};
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r')) {
        value.remove_suffix(1U);
    }
    return value;
}

std::string_view without_comment(std::string_view value) {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const auto character = value[index];
        if (escaped) {
            escaped = false;
        } else if (character == '\\' && quoted) {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        } else if (character == '#' && !quoted) {
            return value.substr(0U, index);
        }
    }
    return value;
}

std::optional<std::string> quoted_string(std::string_view value) {
    value = trim(value);
    if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
        return std::nullopt;
    }
    std::string result;
    result.reserve(value.size() - 2U);
    for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
        const auto character = value[index];
        if (character != '\\') {
            result.push_back(character);
            continue;
        }
        if (++index + 1U > value.size()) {
            return std::nullopt;
        }
        switch (value[index]) {
        case '"':
        case '\\':
            result.push_back(value[index]);
            break;
        case 'n':
            result.push_back('\n');
            break;
        case 't':
            result.push_back('\t');
            break;
        default:
            return std::nullopt;
        }
    }
    return result;
}

std::optional<float> number(std::string_view value) {
    value = trim(value);
    float result = 0.0F;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result,
                                        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::uint32_t> unsigned_number(std::string_view value) {
    value = trim(value);
    std::uint64_t parsed_value = 0U;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), parsed_value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        parsed_value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed_value);
}

std::optional<bool> boolean(std::string_view value) {
    value = trim(value);
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> string_array(std::string_view value) {
    value = trim(value);
    if (value.size() < 2U || value.front() != '[' || value.back() != ']') {
        return std::nullopt;
    }
    value = trim(value.substr(1U, value.size() - 2U));
    std::vector<std::string> result;
    while (!value.empty()) {
        bool escaped = false;
        std::size_t end = 1U;
        if (value.front() != '"') {
            return std::nullopt;
        }
        for (; end < value.size(); ++end) {
            if (escaped) {
                escaped = false;
            } else if (value[end] == '\\') {
                escaped = true;
            } else if (value[end] == '"') {
                break;
            }
        }
        if (end >= value.size()) {
            return std::nullopt;
        }
        auto item = quoted_string(value.substr(0U, end + 1U));
        if (!item) {
            return std::nullopt;
        }
        result.push_back(std::move(*item));
        value = trim(value.substr(end + 1U));
        if (value.empty()) {
            break;
        }
        if (value.front() != ',') {
            return std::nullopt;
        }
        value = trim(value.substr(1U));
    }
    return result;
}

bool array_is_closed(std::string_view value) {
    bool quoted = false;
    bool escaped = false;
    for (const char character : value) {
        if (escaped) {
            escaped = false;
        } else if (quoted && character == '\\') {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        } else if (!quoted && character == ']') {
            return true;
        }
    }
    return false;
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::optional<Color> parse_color(std::string_view value) {
    const auto string = quoted_string(value);
    if (!string || (string->size() != 7U && string->size() != 9U) || string->front() != '#') {
        return std::nullopt;
    }
    std::array<float, 4U> channels{0.0F, 0.0F, 0.0F, 1.0F};
    for (std::size_t channel = 0U; channel < (string->size() - 1U) / 2U; ++channel) {
        const auto high = hex_digit((*string)[channel * 2U + 1U]);
        const auto low = hex_digit((*string)[channel * 2U + 2U]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        channels[channel] = static_cast<float>(high * 16 + low) / 255.0F;
    }
    return Color{channels[0], channels[1], channels[2], channels[3]};
}

bool safe_relative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    return std::ranges::none_of(path, [](const auto& component) {
        return component.empty() || component == "." || component == "..";
    });
}

bool contained_by(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto mismatch = std::ranges::mismatch(root, path);
    return mismatch.in1 == root.end();
}

std::expected<ImageRgba, Diagnostic>
crop_image(const ImageRgba& source, const SpriteSheetRegion& region,
           const std::filesystem::path& theme_path, std::size_t line) {
    if (region.width == 0U || region.height == 0U ||
        static_cast<std::uint64_t>(region.x) + region.width > source.width ||
        static_cast<std::uint64_t>(region.y) + region.height > source.height) {
        return std::unexpected(theme_error(theme_path, line,
                                           "sprite-sheet region is outside its PNG"));
    }
    ImageRgba result{region.width, region.height,
                     std::vector<std::uint8_t>(
                         static_cast<std::size_t>(region.width) * region.height * 4U)};
    for (std::uint32_t row = 0U; row < region.height; ++row) {
        const auto source_offset =
            (static_cast<std::size_t>(region.y + row) * source.width + region.x) * 4U;
        const auto destination_offset =
            static_cast<std::size_t>(row) * region.width * 4U;
        std::ranges::copy_n(source.pixels.begin() +
                                static_cast<std::ptrdiff_t>(source_offset),
                            static_cast<std::ptrdiff_t>(region.width * 4U),
                            result.pixels.begin() +
                                static_cast<std::ptrdiff_t>(destination_offset));
    }
    return result;
}

std::expected<std::filesystem::path, Diagnostic>
validated_asset_path(const std::filesystem::path& package_root,
                     const std::filesystem::path& source,
                     std::span<const std::string> declared,
                     const std::filesystem::path& theme_path, std::size_t line) {
    if (!safe_relative(source)) {
        return std::unexpected(theme_error(theme_path, line,
                                           "visual asset path escapes the package"));
    }
    const auto normalized_source = source.generic_string();
    if (std::ranges::find(declared, normalized_source) == declared.end()) {
        return std::unexpected(theme_error(theme_path, line,
                                           "visual asset is not declared in package assets: " +
                                               normalized_source));
    }
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(package_root, error);
    if (error) {
        return std::unexpected(theme_error(theme_path, line,
                                           "unable to resolve package root: " + error.message()));
    }
    const auto candidate = std::filesystem::canonical(canonical_root / source, error);
    if (error || !contained_by(canonical_root, candidate)) {
        return std::unexpected(theme_error(theme_path, line,
                                           "visual asset is missing or resolves outside the package"));
    }
    if (!std::filesystem::is_regular_file(candidate, error) || error) {
        return std::unexpected(
            theme_error(theme_path, line, "visual asset is not a regular file"));
    }
    return candidate;
}

} // namespace

std::expected<VisualTheme, Diagnostic>
VisualTheme::load_package(const std::filesystem::path& package_root,
                          const std::filesystem::path& manifest_relative) {
    if (!safe_relative(manifest_relative)) {
        return std::unexpected(theme_error(manifest_relative, 0U,
                                           "package manifest path is unsafe"));
    }
    const auto manifest_path = package_root / manifest_relative;
    std::error_code error;
    const auto manifest_size = std::filesystem::file_size(manifest_path, error);
    if (error || manifest_size > (1U << 20U)) {
        return std::unexpected(theme_error(manifest_path, 0U,
                                           "unable to inspect bounded package manifest"));
    }
    std::ifstream input{manifest_path, std::ios::binary};
    if (!input) {
        return std::unexpected(theme_error(manifest_path, 0U,
                                           "unable to open package manifest"));
    }
    bool package_section = false;
    std::optional<std::string> visuals;
    std::vector<std::string> assets;
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        const auto view = trim(without_comment(line));
        if (view.empty()) {
            continue;
        }
        if (view.front() == '[') {
            package_section = view == "[package]";
            continue;
        }
        if (!package_section) {
            continue;
        }
        const auto separator = view.find('=');
        if (separator == std::string_view::npos) {
            continue;
        }
        const auto key = trim(view.substr(0U, separator));
        const auto value = trim(view.substr(separator + 1U));
        if (key == "visuals") {
            visuals = quoted_string(value);
            if (!visuals) {
                return std::unexpected(theme_error(manifest_path, line_number,
                                                   "package visuals must be a string path"));
            }
        } else if (key == "assets") {
            std::string array_text{value};
            while (!array_is_closed(array_text) && std::getline(input, line)) {
                ++line_number;
                const auto continuation = trim(without_comment(line));
                array_text.push_back(' ');
                array_text.append(continuation);
            }
            const auto parsed = string_array(array_text);
            if (!parsed) {
                return std::unexpected(theme_error(manifest_path, line_number,
                                                   "package assets must be a string array"));
            }
            assets = *parsed;
        }
    }
    if (!visuals) {
        return std::unexpected(theme_error(manifest_path, 0U,
                                           "package does not declare a visual theme"));
    }
    if (assets.size() > maximum_assets) {
        return std::unexpected(theme_error(manifest_path, 0U,
                                           "package exceeds 1024 declared visual assets"));
    }
    std::set<std::string> declared;
    for (const auto& asset : assets) {
        if (!safe_relative(asset) || !declared.insert(asset).second) {
            return std::unexpected(theme_error(
                manifest_path, 0U,
                "package assets contain an unsafe or duplicate path: " + asset));
        }
    }
    return load(package_root, *visuals, assets);
}

std::expected<VisualTheme, Diagnostic>
VisualTheme::load(const std::filesystem::path& package_root,
                  const std::filesystem::path& theme_relative,
                  std::span<const std::string> declared_assets) {
    if (!safe_relative(theme_relative)) {
        return std::unexpected(theme_error(theme_relative, 0U,
                                           "theme path must stay inside the package"));
    }
    std::error_code root_error;
    const auto canonical_root = std::filesystem::weakly_canonical(package_root, root_error);
    if (root_error) {
        return std::unexpected(theme_error(theme_relative, 0U,
                                           "unable to resolve package root: " +
                                               root_error.message()));
    }
    std::error_code theme_path_error;
    const auto theme_path =
        std::filesystem::canonical(canonical_root / theme_relative, theme_path_error);
    if (theme_path_error || !contained_by(canonical_root, theme_path)) {
        return std::unexpected(theme_error(theme_relative, 0U,
                                           "theme is missing or resolves outside the package"));
    }
    std::error_code error;
    const auto theme_size = std::filesystem::file_size(theme_path, error);
    if (error || theme_size > (1U << 20U)) {
        return std::unexpected(
            theme_error(theme_path, 0U, "theme file exceeds the 1 MiB text limit"));
    }
    std::ifstream input{theme_path, std::ios::binary};
    if (!input) {
        return std::unexpected(theme_error(theme_path, 0U, "unable to open visual theme"));
    }

    enum class Section { none, theme, sprite, animation, color } section{Section::none};
    VisualTheme result;
    std::vector<ParsedSprite> parsed_sprites;
    std::vector<ParsedAnimation> parsed_animations;
    std::vector<ParsedColor> parsed_colors;
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        auto view = trim(without_comment(line));
        if (view.empty()) {
            continue;
        }
        if (view == "[theme]") {
            section = Section::theme;
            continue;
        }
        if (view == "[[sprite]]") {
            if (parsed_sprites.size() >= maximum_assets) {
                return std::unexpected(theme_error(theme_path, line_number,
                                                   "theme exceeds 1024 sprites"));
            }
            parsed_sprites.emplace_back();
            parsed_sprites.back().line = line_number;
            section = Section::sprite;
            continue;
        }
        if (view == "[[animation]]") {
            parsed_animations.push_back(ParsedAnimation{{}, {}, 100U, true, line_number});
            section = Section::animation;
            continue;
        }
        if (view == "[[color]]") {
            if (parsed_colors.size() >= 128U) {
                return std::unexpected(theme_error(theme_path, line_number,
                                                   "theme exceeds 128 named colors"));
            }
            parsed_colors.push_back(ParsedColor{{}, {}, false, line_number});
            section = Section::color;
            continue;
        }
        const auto separator = view.find('=');
        if (separator == std::string_view::npos || section == Section::none) {
            return std::unexpected(theme_error(theme_path, line_number,
                                               "expected a supported theme table or key/value"));
        }
        const auto key = trim(view.substr(0U, separator));
        const auto value = trim(view.substr(separator + 1U));
        bool recognized = true;
        if (section == Section::theme) {
            if (key == "id") {
                auto parsed = quoted_string(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    result.id_ = std::move(*parsed);
                }
            } else if (key == "display_name") {
                auto parsed = quoted_string(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    result.display_name_ = std::move(*parsed);
                }
            } else if (key == "font_family") {
                auto parsed = quoted_string(value);
                if (!parsed || parsed->empty()) {
                    recognized = false;
                } else {
                    result.font_families_ = {std::move(*parsed)};
                }
            } else if (key == "font_families") {
                auto parsed = string_array(value);
                if (!parsed || parsed->empty() || parsed->size() > 8U ||
                    std::ranges::any_of(*parsed, [](const std::string& family) {
                        return family.empty() || family.size() > 128U;
                    })) {
                    recognized = false;
                } else {
                    result.font_families_ = std::move(*parsed);
                }
            } else if (key == "background") {
                auto parsed = parse_color(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    result.background_ = *parsed;
                }
            } else {
                recognized = false;
            }
        } else if (section == Section::sprite) {
            auto& sprite = parsed_sprites.back();
            if (key == "id") {
                auto parsed = quoted_string(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    sprite.id = std::move(*parsed);
                }
            } else if (key == "source") {
                auto parsed = quoted_string(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    sprite.source = std::move(*parsed);
                }
            } else if (key == "pivot_x" || key == "pivot_y" || key == "world_width" ||
                       key == "world_height") {
                const auto parsed = number(value);
                if (!parsed) {
                    recognized = false;
                } else if (key == "pivot_x") {
                    sprite.pivot.x = *parsed;
                } else if (key == "pivot_y") {
                    sprite.pivot.y = *parsed;
                } else if (key == "world_width") {
                    sprite.size.x = *parsed;
                } else {
                    sprite.size.y = *parsed;
                }
            } else if (key == "filter") {
                const auto parsed = quoted_string(value);
                if (!parsed || (*parsed != "linear" && *parsed != "nearest")) {
                    recognized = false;
                } else {
                    sprite.filter = *parsed == "nearest" ? TextureFilter::nearest
                                                          : TextureFilter::linear;
                }
            } else if (key == "region_x" || key == "region_y" ||
                       key == "region_width" || key == "region_height") {
                const auto parsed = unsigned_number(value);
                if (!parsed) {
                    recognized = false;
                } else if (key == "region_x") {
                    sprite.region_x = *parsed;
                } else if (key == "region_y") {
                    sprite.region_y = *parsed;
                } else if (key == "region_width") {
                    sprite.region_width = *parsed;
                } else {
                    sprite.region_height = *parsed;
                }
            } else {
                recognized = false;
            }
        } else if (section == Section::animation) {
            auto& animation = parsed_animations.back();
            if (key == "id") {
                auto parsed = quoted_string(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    animation.id = std::move(*parsed);
                }
            } else if (key == "frames") {
                auto parsed = string_array(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    animation.frame_ids = std::move(*parsed);
                }
            } else if (key == "frame_ms") {
                const auto parsed = unsigned_number(value);
                if (!parsed || *parsed == 0U || *parsed > 60'000U) {
                    recognized = false;
                } else {
                    animation.frame_milliseconds = *parsed;
                }
            } else if (key == "loop") {
                const auto parsed = boolean(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    animation.loop = *parsed;
                }
            } else {
                recognized = false;
            }
        } else {
            auto& parsed_color = parsed_colors.back();
            if (key == "id") {
                auto parsed = quoted_string(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    parsed_color.id = std::move(*parsed);
                }
            } else if (key == "value") {
                auto parsed = parse_color(value);
                if (!parsed) {
                    recognized = false;
                } else {
                    parsed_color.value = *parsed;
                    parsed_color.has_value = true;
                }
            } else {
                recognized = false;
            }
        }
        if (!recognized) {
            return std::unexpected(theme_error(theme_path, line_number,
                                               "unknown key or invalid value: " +
                                                   std::string{key}));
        }
    }
    if (!input.eof()) {
        return std::unexpected(theme_error(theme_path, line_number,
                                           "unable to read complete visual theme"));
    }
    if (result.id_.empty() || parsed_sprites.empty()) {
        return std::unexpected(theme_error(theme_path, 0U,
                                           "theme requires an id and at least one sprite"));
    }
    if (result.display_name_.empty()) {
        result.display_name_ = result.id_;
    }

    std::set<std::string> sprite_ids;
    std::vector<ImageRgba> images;
    images.reserve(parsed_sprites.size());
    result.sprites_.reserve(parsed_sprites.size());
    std::uint64_t decoded_pixels = 0U;
    std::vector<std::pair<std::filesystem::path, ImageRgba>> decoded_sources;
    for (std::size_t index = 0U; index < parsed_sprites.size(); ++index) {
        const auto& sprite = parsed_sprites[index];
        if (sprite.id.empty() || sprite.source.empty() || !sprite_ids.insert(sprite.id).second ||
            sprite.pivot.x < 0.0F || sprite.pivot.x > 1.0F || sprite.pivot.y < 0.0F ||
            sprite.pivot.y > 1.0F || sprite.size.x <= 0.0F || sprite.size.x > 64.0F ||
            sprite.size.y <= 0.0F || sprite.size.y > 64.0F) {
            return std::unexpected(theme_error(theme_path, sprite.line,
                                               "sprite is incomplete, duplicated, or out of range"));
        }
        auto source_path = validated_asset_path(package_root, sprite.source, declared_assets,
                                                theme_path, sprite.line);
        if (!source_path) {
            return std::unexpected(source_path.error());
        }
        auto decoded = std::ranges::find(decoded_sources, *source_path,
                                         &std::pair<std::filesystem::path, ImageRgba>::first);
        if (decoded == decoded_sources.end()) {
            auto source_image = load_png_rgba(*source_path);
            if (!source_image) {
                return std::unexpected(source_image.error());
            }
            decoded_pixels += static_cast<std::uint64_t>(source_image->width) *
                              source_image->height;
            if (decoded_pixels > maximum_decoded_pixels) {
                return std::unexpected(theme_error(theme_path, sprite.line,
                                                   "theme exceeds 64 million decoded pixels"));
            }
            decoded_sources.emplace_back(*source_path, std::move(*source_image));
            decoded = std::prev(decoded_sources.end());
        }
        const auto region_fields = static_cast<unsigned>(sprite.region_x.has_value()) +
                                   static_cast<unsigned>(sprite.region_y.has_value()) +
                                   static_cast<unsigned>(sprite.region_width.has_value()) +
                                   static_cast<unsigned>(sprite.region_height.has_value());
        std::optional<SpriteSheetRegion> source_region;
        if (region_fields != 0U && region_fields != 4U) {
            return std::unexpected(theme_error(
                theme_path, sprite.line,
                "sprite-sheet region requires x, y, width, and height"));
        }
        if (region_fields == 4U) {
            source_region = SpriteSheetRegion{*sprite.region_x, *sprite.region_y,
                                              *sprite.region_width,
                                              *sprite.region_height};
            auto cropped = crop_image(decoded->second, *source_region, theme_path,
                                      sprite.line);
            if (!cropped) {
                return std::unexpected(cropped.error());
            }
            images.push_back(std::move(*cropped));
        } else {
            images.push_back(decoded->second);
        }
        result.sprites_.push_back(ThemeSprite{sprite.id, sprite.source,
                                              SpriteId{static_cast<std::uint32_t>(index)},
                                              sprite.pivot, sprite.size, sprite.filter,
                                              source_region});
    }
    auto atlas = TextureAtlas::pack(images, 2U);
    if (!atlas) {
        return std::unexpected(atlas.error());
    }
    for (std::size_t index = 0U; index < parsed_sprites.size(); ++index) {
        atlas->set_nearest(SpriteId{static_cast<std::uint32_t>(index)},
                           parsed_sprites[index].filter == TextureFilter::nearest);
    }
    result.atlas_ = std::move(*atlas);

    std::set<std::string> animation_ids;
    result.animations_.reserve(parsed_animations.size());
    for (const auto& animation : parsed_animations) {
        if (animation.id.empty() || animation.frame_ids.empty() ||
            !animation_ids.insert(animation.id).second) {
            return std::unexpected(theme_error(theme_path, animation.line,
                                               "animation is incomplete or duplicated"));
        }
        ThemeAnimation resolved{animation.id, {}, animation.frame_milliseconds, animation.loop};
        resolved.frames.reserve(animation.frame_ids.size());
        for (const auto& frame : animation.frame_ids) {
            const auto sprite_id = result.sprite(frame);
            if (!sprite_id) {
                return std::unexpected(theme_error(theme_path, animation.line,
                                                   "animation references missing sprite: " + frame));
            }
            resolved.frames.push_back(*sprite_id);
        }
        result.animations_.push_back(std::move(resolved));
    }
    std::set<std::string> color_ids;
    result.colors_.reserve(parsed_colors.size());
    for (const auto& named : parsed_colors) {
        if (named.id.empty() || named.id.size() > 128U || !named.has_value ||
            !color_ids.insert(named.id).second) {
            return std::unexpected(theme_error(theme_path, named.line,
                                               "named color is incomplete or duplicated"));
        }
        result.colors_.push_back(ThemeColor{named.id, named.value});
    }
    return result;
}

std::optional<SpriteId> VisualTheme::sprite(std::string_view id) const noexcept {
    const auto found = std::ranges::find(sprites_, id, &ThemeSprite::id);
    return found == sprites_.end() ? std::nullopt : std::optional<SpriteId>{found->sprite};
}

std::optional<Color> VisualTheme::color(std::string_view id) const noexcept {
    const auto found = std::ranges::find(colors_, id, &ThemeColor::id);
    return found == colors_.end() ? std::nullopt : std::optional<Color>{found->value};
}

const ThemeAnimation* VisualTheme::animation(std::string_view id) const noexcept {
    const auto found = std::ranges::find(animations_, id, &ThemeAnimation::id);
    return found == animations_.end() ? nullptr : &*found;
}

std::expected<AssetCatalog, Diagnostic>
AssetCatalog::load_package(std::filesystem::path package_root,
                           std::filesystem::path manifest) {
    auto theme = VisualTheme::load_package(package_root, manifest);
    if (!theme) {
        return std::unexpected(theme.error());
    }
    return AssetCatalog{std::move(package_root), std::move(manifest), std::move(*theme)};
}

std::expected<bool, Diagnostic> AssetCatalog::reload() {
    auto candidate = VisualTheme::load_package(package_root_, manifest_);
    if (!candidate) {
        last_diagnostic_ = candidate.error();
        return std::unexpected(candidate.error());
    }
    theme_ = std::move(*candidate);
    last_diagnostic_.reset();
    ++generation_;
    return true;
}

} // namespace ludus
