#include "ludus/render/atlas.hpp"

#include <png.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace ludus {
namespace {

constexpr std::uint64_t bytes_per_pixel = 4U;
constexpr std::uintmax_t maximum_compressed_image_size = 16U << 20U;

Diagnostic atlas_error(std::string message) {
    return Diagnostic{DiagnosticCode::validation_failed, std::move(message), {}};
}

Diagnostic image_error(const std::filesystem::path& path, std::string message) {
    return Diagnostic{DiagnosticCode::serialization_error, std::move(message),
                      SourceLocation{path.string(), 0U, 0U}};
}

std::uint32_t next_power_of_two(std::uint32_t value) noexcept {
    if (value <= 1U) {
        return 1U;
    }
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
}

std::uint8_t premultiply_srgb(std::uint8_t channel, std::uint8_t alpha) {
    const auto encoded = static_cast<double>(channel) / 255.0;
    const auto linear = encoded <= 0.04045
                            ? encoded / 12.92
                            : std::pow((encoded + 0.055) / 1.055, 2.4);
    const auto premultiplied = linear * (static_cast<double>(alpha) / 255.0);
    const auto encoded_result = premultiplied <= 0.0031308
                                    ? premultiplied * 12.92
                                    : 1.055 * std::pow(premultiplied, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(encoded_result * 255.0), 0L, 255L));
}

std::optional<std::string> read_ppm_token(std::istream& stream) {
    std::string token;
    char character = '\0';
    while (stream.get(character)) {
        if (character == '#') {
            stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(character))) {
            token.push_back(character);
            break;
        }
    }
    while (stream.get(character)) {
        if (character == '#') {
            stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            break;
        }
        if (std::isspace(static_cast<unsigned char>(character))) {
            break;
        }
        token.push_back(character);
    }
    return token.empty() ? std::nullopt : std::optional<std::string>{std::move(token)};
}

std::optional<std::uint32_t> parse_unsigned(const std::optional<std::string>& token) {
    if (!token || token->empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0U;
    for (const char character : *token) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

struct Placement {
    std::uint32_t x{0U};
    std::uint32_t y{0U};
};

std::optional<std::pair<std::vector<Placement>, std::uint32_t>>
try_pack(std::span<const ImageRgba> images, std::uint32_t atlas_width,
         std::uint32_t padding, std::uint32_t maximum_height) {
    std::vector<Placement> placements;
    placements.reserve(images.size());
    std::uint32_t x = padding;
    std::uint32_t y = padding;
    std::uint32_t shelf_height = 0U;
    for (const auto& image : images) {
        if (image.width + padding * 2U > atlas_width) {
            return std::nullopt;
        }
        if (x + image.width + padding > atlas_width) {
            x = padding;
            y += shelf_height + padding;
            shelf_height = 0U;
        }
        if (y + image.height + padding > maximum_height) {
            return std::nullopt;
        }
        placements.push_back(Placement{x, y});
        x += image.width + padding;
        shelf_height = std::max(shelf_height, image.height);
    }
    return std::pair{std::move(placements), y + shelf_height + padding};
}

AtlasRegion make_region(SpriteId sprite, std::uint32_t page, std::uint32_t x, std::uint32_t y,
                        std::uint32_t width, std::uint32_t height,
                        std::uint32_t atlas_width, std::uint32_t atlas_height) {
    const auto atlas_width_float = static_cast<float>(atlas_width);
    const auto atlas_height_float = static_cast<float>(atlas_height);
    constexpr float half_texel = 0.5F;
    return AtlasRegion{sprite,
                       page,
                       x,
                       y,
                       width,
                       height,
                       (static_cast<float>(x) + half_texel) / atlas_width_float,
                       (static_cast<float>(y) + half_texel) / atlas_height_float,
                       (static_cast<float>(x + width) - half_texel) / atlas_width_float,
                       (static_cast<float>(y + height) - half_texel) / atlas_height_float,
                       false};
}

struct PackedPage {
    ImageRgba image;
    std::vector<AtlasRegion> regions;
};

std::expected<PackedPage, Diagnostic>
build_page(std::span<const ImageRgba> images, std::uint32_t padding,
           std::uint32_t first_sprite, std::uint32_t page,
           std::uint32_t page_dimension) {
    std::uint64_t area = 0U;
    std::uint32_t widest = 1U;
    for (const auto& image : images) {
        const auto padded_width = image.width + padding * 2U;
        const auto padded_height = image.height + padding * 2U;
        area += static_cast<std::uint64_t>(padded_width) * padded_height;
        widest = std::max(widest, padded_width);
    }
    const auto area_side = static_cast<std::uint32_t>(
        std::ceil(std::sqrt(static_cast<double>(area))));
    auto atlas_width = next_power_of_two(std::max(widest, area_side));
    atlas_width = std::min(atlas_width, page_dimension);

    std::optional<std::pair<std::vector<Placement>, std::uint32_t>> packed;
    while (atlas_width <= page_dimension) {
        packed = try_pack(images, atlas_width, padding, page_dimension);
        if (packed && next_power_of_two(packed->second) <= page_dimension) {
            break;
        }
        if (atlas_width == page_dimension) {
            packed.reset();
            break;
        }
        atlas_width = std::min(atlas_width * 2U, page_dimension);
    }
    if (!packed) {
        return std::unexpected(atlas_error("texture atlas page exceeds the maximum dimensions"));
    }

    const auto atlas_height = next_power_of_two(packed->second);
    PackedPage result;
    result.image.width = atlas_width;
    result.image.height = atlas_height;
    result.image.pixels.resize(static_cast<std::size_t>(atlas_width) * atlas_height *
                               bytes_per_pixel);
    result.regions.reserve(images.size());
    for (std::size_t index = 0U; index < images.size(); ++index) {
        const auto& source = images[index];
        const auto placement = packed->first[index];
        // Copy the sprite and extrude its edge texels through the padding. This
        // prevents linear-filter sampling from bleeding transparent neighboring data.
        for (std::uint32_t padded_row = 0U;
             padded_row < source.height + padding * 2U; ++padded_row) {
            const auto source_row = padded_row < padding
                                        ? 0U
                                    : padded_row - padding >= source.height
                                        ? source.height - 1U
                                        : padded_row - padding;
            const auto destination_row = placement.y - padding + padded_row;
            for (std::uint32_t padded_column = 0U;
                 padded_column < source.width + padding * 2U; ++padded_column) {
                const auto source_column = padded_column < padding
                                               ? 0U
                                           : padded_column - padding >= source.width
                                               ? source.width - 1U
                                               : padded_column - padding;
                const auto destination_column = placement.x - padding + padded_column;
                const auto source_offset =
                    (static_cast<std::size_t>(source_row) * source.width + source_column) *
                    bytes_per_pixel;
                const auto destination_offset =
                    (static_cast<std::size_t>(destination_row) * atlas_width +
                     destination_column) *
                    bytes_per_pixel;
                const auto alpha = source.pixels[source_offset + 3U];
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    result.image.pixels[destination_offset + channel] = premultiply_srgb(
                        source.pixels[source_offset + channel], alpha);
                }
                result.image.pixels[destination_offset + 3U] = alpha;
            }
        }
        result.regions.push_back(make_region(
            SpriteId{first_sprite + static_cast<std::uint32_t>(index)}, page,
            placement.x, placement.y, source.width, source.height, atlas_width,
            atlas_height));
    }
    return result;
}

} // namespace

bool ImageRgba::valid() const noexcept {
    if (width == 0U || height == 0U || width > TextureAtlas::maximum_dimension ||
        height > TextureAtlas::maximum_dimension) {
        return false;
    }
    const auto expected = static_cast<std::uint64_t>(width) * height * bytes_per_pixel;
    return expected == pixels.size();
}

std::expected<TextureAtlas, Diagnostic>
TextureAtlas::pack(std::span<const ImageRgba> images, std::uint32_t padding,
                   std::uint32_t page_dimension) {
    if (images.empty() || padding > 64U || page_dimension < 16U ||
        page_dimension > maximum_dimension ||
        (page_dimension & (page_dimension - 1U)) != 0U ||
        std::ranges::any_of(images, [](const ImageRgba& image) { return !image.valid(); })) {
        return std::unexpected(atlas_error("texture atlas input is empty or invalid"));
    }

    TextureAtlas result;
    result.regions_.reserve(images.size());
    std::size_t first = 0U;
    while (first < images.size()) {
        std::size_t count = 0U;
        for (std::size_t candidate = 1U; first + candidate <= images.size(); ++candidate) {
            const auto layout = try_pack(images.subspan(first, candidate), page_dimension,
                                         padding, page_dimension);
            if (!layout || next_power_of_two(layout->second) > page_dimension) {
                break;
            }
            count = candidate;
        }
        if (count == 0U || result.pages_.size() >= maximum_pages) {
            return std::unexpected(atlas_error(
                count == 0U ? "texture atlas contains an image too large for padded packing"
                            : "texture atlas exceeds the 16-page limit"));
        }
        const auto page_index = static_cast<std::uint32_t>(result.pages_.size());
        auto page = build_page(images.subspan(first, count), padding,
                               static_cast<std::uint32_t>(first), page_index,
                               page_dimension);
        if (!page) {
            return std::unexpected(page.error());
        }
        result.pages_.push_back(std::move(page->image));
        std::ranges::move(page->regions, std::back_inserter(result.regions_));
        first += count;
    }
    std::uint32_t common_width = 0U;
    std::uint32_t common_height = 0U;
    for (const auto& page : result.pages_) {
        common_width = std::max(common_width, page.width);
        common_height = std::max(common_height, page.height);
    }
    for (auto& page : result.pages_) {
        if (page.width == common_width && page.height == common_height) {
            continue;
        }
        ImageRgba normalized{common_width, common_height,
                             std::vector<std::uint8_t>(
                                 static_cast<std::size_t>(common_width) * common_height *
                                     bytes_per_pixel,
                                 0U)};
        for (std::uint32_t row = 0U; row < page.height; ++row) {
            const auto source_offset =
                static_cast<std::size_t>(row) * page.width * bytes_per_pixel;
            const auto destination_offset =
                static_cast<std::size_t>(row) * common_width * bytes_per_pixel;
            std::ranges::copy_n(
                page.pixels.begin() + static_cast<std::ptrdiff_t>(source_offset),
                static_cast<std::ptrdiff_t>(page.width * bytes_per_pixel),
                normalized.pixels.begin() +
                    static_cast<std::ptrdiff_t>(destination_offset));
        }
        page = std::move(normalized);
    }
    for (auto& region : result.regions_) {
        const auto normalized = make_region(region.sprite, region.page, region.x, region.y,
                                            region.width, region.height, common_width,
                                            common_height);
        region = normalized;
    }
    return result;
}

std::expected<TextureAtlas, Diagnostic>
TextureAtlas::from_grid(ImageRgba image, std::uint32_t columns, std::uint32_t rows) {
    if (!image.valid() || columns == 0U || rows == 0U || image.width % columns != 0U ||
        image.height % rows != 0U ||
        static_cast<std::uint64_t>(columns) * rows > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(atlas_error("texture atlas grid is invalid"));
    }
    for (std::size_t offset = 0U; offset < image.pixels.size(); offset += 4U) {
        const auto alpha = image.pixels[offset + 3U];
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            image.pixels[offset + channel] =
                premultiply_srgb(image.pixels[offset + channel], alpha);
        }
    }
    TextureAtlas result;
    result.pages_.push_back(std::move(image));
    const auto cell_width = result.pages_.front().width / columns;
    const auto cell_height = result.pages_.front().height / rows;
    result.regions_.reserve(static_cast<std::size_t>(columns) * rows);
    for (std::uint32_t row = 0U; row < rows; ++row) {
        for (std::uint32_t column = 0U; column < columns; ++column) {
            const auto index = row * columns + column;
            result.regions_.push_back(make_region(SpriteId{index}, 0U, column * cell_width,
                                                  row * cell_height, cell_width, cell_height,
                                                  result.pages_.front().width,
                                                  result.pages_.front().height));
        }
    }
    return result;
}

const AtlasRegion* TextureAtlas::region(SpriteId sprite) const noexcept {
    if (sprite.value >= regions_.size()) {
        return nullptr;
    }
    return &regions_[sprite.value];
}

void TextureAtlas::set_nearest(SpriteId sprite, bool nearest) noexcept {
    if (sprite.value < regions_.size()) {
        regions_[sprite.value].nearest = nearest;
    }
}

std::expected<ImageRgba, Diagnostic> load_ppm_rgba(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::unexpected(image_error(path, "unable to open texture image"));
    }
    const auto magic = read_ppm_token(stream);
    const auto width = parse_unsigned(read_ppm_token(stream));
    const auto height = parse_unsigned(read_ppm_token(stream));
    const auto maximum = parse_unsigned(read_ppm_token(stream));
    if (!magic || (*magic != "P6" && *magic != "P3") || !width || !height || !maximum ||
        *width == 0U || *height == 0U || *width > TextureAtlas::maximum_dimension ||
        *height > TextureAtlas::maximum_dimension || *maximum == 0U || *maximum > 255U) {
        return std::unexpected(image_error(path, "invalid or unsupported PPM header"));
    }

    const auto pixel_count = static_cast<std::size_t>(*width) * *height;
    ImageRgba result{*width, *height, {}};
    result.pixels.resize(pixel_count * bytes_per_pixel);
    if (*magic == "P6") {
        std::vector<std::uint8_t> rgb(pixel_count * 3U);
        stream.read(reinterpret_cast<char*>(rgb.data()),
                    static_cast<std::streamsize>(rgb.size()));
        if (stream.gcount() != static_cast<std::streamsize>(rgb.size())) {
            return std::unexpected(image_error(path, "PPM pixel data is truncated"));
        }
        for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                result.pixels[pixel * 4U + channel] = static_cast<std::uint8_t>(
                    static_cast<std::uint32_t>(rgb[pixel * 3U + channel]) * 255U / *maximum);
            }
            result.pixels[pixel * 4U + 3U] = 255U;
        }
    } else {
        for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const auto value = parse_unsigned(read_ppm_token(stream));
                if (!value || *value > *maximum) {
                    return std::unexpected(image_error(path, "PPM pixel data is invalid"));
                }
                result.pixels[pixel * 4U + channel] =
                    static_cast<std::uint8_t>(*value * 255U / *maximum);
            }
            result.pixels[pixel * 4U + 3U] = 255U;
        }
    }
    return result;
}

std::expected<ImageRgba, Diagnostic> load_png_rgba(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const auto compressed_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(image_error(path, "unable to inspect PNG image: " +
                                                     filesystem_error.message()));
    }
    if (compressed_size == 0U || compressed_size > maximum_compressed_image_size) {
        return std::unexpected(
            image_error(path, "PNG image is empty or exceeds the 16 MiB package limit"));
    }

    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    const auto filename = path.string();
    if (png_image_begin_read_from_file(&image, filename.c_str()) == 0) {
        return std::unexpected(image_error(path, "unable to read PNG header: " +
                                                     std::string{image.message}));
    }
    const auto release = [&image] { png_image_free(&image); };
    if (image.width == 0U || image.height == 0U ||
        image.width > TextureAtlas::maximum_dimension ||
        image.height > TextureAtlas::maximum_dimension) {
        release();
        return std::unexpected(
            image_error(path, "PNG dimensions must be between 1 and 4096 pixels"));
    }
    image.format = PNG_FORMAT_RGBA;
    const auto byte_count = PNG_IMAGE_SIZE(image);
    if (byte_count == 0U || byte_count > std::numeric_limits<std::size_t>::max()) {
        release();
        return std::unexpected(image_error(path, "PNG decoded size is invalid"));
    }
    ImageRgba result{image.width, image.height, {}};
    result.pixels.resize(static_cast<std::size_t>(byte_count));
    if (png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr) == 0) {
        const auto message = std::string{image.message};
        release();
        return std::unexpected(image_error(path, "unable to decode PNG image: " + message));
    }
    release();
    return result;
}

} // namespace ludus
