#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/render/atlas.hpp"
#include "ludus/render/math.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ludus {

enum class TextureFilter : std::uint8_t { linear, nearest };

struct SpriteSheetRegion {
    std::uint32_t x{0U};
    std::uint32_t y{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};

    auto operator<=>(const SpriteSheetRegion&) const = default;
};

struct ThemeSprite {
    std::string id;
    std::filesystem::path source;
    SpriteId sprite;
    Vec2 pivot{0.5F, 0.5F};
    Vec2 world_size{1.0F, 1.0F};
    TextureFilter filter{TextureFilter::linear};
    std::optional<SpriteSheetRegion> source_region;

    auto operator<=>(const ThemeSprite&) const = default;
};

struct ThemeAnimation {
    std::string id;
    std::vector<SpriteId> frames;
    std::uint32_t frame_milliseconds{100U};
    bool loop{true};

    auto operator<=>(const ThemeAnimation&) const = default;
};

struct ThemeColor {
    std::string id;
    Color value;

    auto operator<=>(const ThemeColor&) const = default;
};

/// Immutable, validated package visual catalog. Names resolve once to compact sprite IDs.
class VisualTheme {
  public:
    static constexpr std::size_t maximum_assets = 1'024U;
    static constexpr std::uint64_t maximum_decoded_pixels = 64'000'000U;

    [[nodiscard]] static std::expected<VisualTheme, Diagnostic>
    load(const std::filesystem::path& package_root,
         const std::filesystem::path& theme_path,
         std::span<const std::string> declared_assets);
    [[nodiscard]] static std::expected<VisualTheme, Diagnostic>
    load_package(const std::filesystem::path& package_root,
                 const std::filesystem::path& manifest = "package.toml");

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& display_name() const noexcept { return display_name_; }
    [[nodiscard]] const std::string& font_family() const noexcept {
        return font_families_.front();
    }
    [[nodiscard]] std::span<const std::string> font_families() const noexcept {
        return font_families_;
    }
    [[nodiscard]] Color background() const noexcept { return background_; }
    [[nodiscard]] const TextureAtlas& atlas() const noexcept { return atlas_; }
    [[nodiscard]] std::span<const ThemeSprite> sprites() const noexcept { return sprites_; }
    [[nodiscard]] std::span<const ThemeAnimation> animations() const noexcept {
        return animations_;
    }
    [[nodiscard]] std::span<const ThemeColor> colors() const noexcept { return colors_; }
    [[nodiscard]] std::optional<SpriteId> sprite(std::string_view id) const noexcept;
    [[nodiscard]] std::optional<Color> color(std::string_view id) const noexcept;
    [[nodiscard]] const ThemeAnimation* animation(std::string_view id) const noexcept;

  private:
    std::string id_;
    std::string display_name_;
    std::vector<std::string> font_families_{"Serif"};
    Color background_{0.035F, 0.039F, 0.052F, 1.0F};
    TextureAtlas atlas_;
    std::vector<ThemeSprite> sprites_;
    std::vector<ThemeAnimation> animations_;
    std::vector<ThemeColor> colors_;
};

/// Hot-reload owner that only publishes a newly validated theme.
class AssetCatalog {
  public:
    [[nodiscard]] static std::expected<AssetCatalog, Diagnostic>
    load_package(std::filesystem::path package_root,
                 std::filesystem::path manifest = "package.toml");

    /// Returns true after atomically replacing the active catalog. On failure the
    /// previous theme remains active and the structured diagnostic is retained.
    [[nodiscard]] std::expected<bool, Diagnostic> reload();

    [[nodiscard]] const VisualTheme& theme() const noexcept { return theme_; }
    [[nodiscard]] std::optional<SpriteId> sprite(std::string_view id) const noexcept {
        return theme_.sprite(id);
    }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::optional<Diagnostic>& last_diagnostic() const noexcept {
        return last_diagnostic_;
    }

  private:
    AssetCatalog(std::filesystem::path package_root, std::filesystem::path manifest,
                 VisualTheme theme)
        : package_root_(std::move(package_root)), manifest_(std::move(manifest)),
          theme_(std::move(theme)) {}

    std::filesystem::path package_root_;
    std::filesystem::path manifest_;
    VisualTheme theme_;
    std::optional<Diagnostic> last_diagnostic_;
    std::uint64_t generation_{1U};
};

} // namespace ludus
