#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/render/snapshot.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ludus {

struct ImageRgba {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool valid() const noexcept;
};

struct AtlasRegion {
    SpriteId sprite;
    std::uint32_t page{0U};
    std::uint32_t x{0U};
    std::uint32_t y{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    float u_min{0.0F};
    float v_min{0.0F};
    float u_max{1.0F};
    float v_max{1.0F};
    bool nearest{false};

    auto operator<=>(const AtlasRegion&) const = default;
};

class TextureAtlas {
  public:
    static constexpr std::uint32_t maximum_dimension = 4'096U;
    static constexpr std::uint32_t maximum_pages = 16U;

    [[nodiscard]] static std::expected<TextureAtlas, Diagnostic>
    pack(std::span<const ImageRgba> images, std::uint32_t padding = 1U,
         std::uint32_t page_dimension = maximum_dimension);
    [[nodiscard]] static std::expected<TextureAtlas, Diagnostic>
    from_grid(ImageRgba image, std::uint32_t columns, std::uint32_t rows);

    /// Compatibility accessor for the first page. New renderers should use pages().
    [[nodiscard]] const ImageRgba& image() const noexcept {
        static const ImageRgba empty;
        return pages_.empty() ? empty : pages_.front();
    }
    [[nodiscard]] std::span<const ImageRgba> pages() const noexcept { return pages_; }
    [[nodiscard]] std::span<const AtlasRegion> regions() const noexcept { return regions_; }
    [[nodiscard]] const AtlasRegion* region(SpriteId sprite) const noexcept;
    void set_nearest(SpriteId sprite, bool nearest) noexcept;

  private:
    std::vector<ImageRgba> pages_;
    std::vector<AtlasRegion> regions_;
};

[[nodiscard]] std::expected<ImageRgba, Diagnostic>
load_ppm_rgba(const std::filesystem::path& path);
[[nodiscard]] std::expected<ImageRgba, Diagnostic>
load_png_rgba(const std::filesystem::path& path);

} // namespace ludus
