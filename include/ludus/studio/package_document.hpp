#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/render/snapshot.hpp"
#include "ludus/render/theme.hpp"
#include "ludus/topology/topology.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ludus::studio {

struct PackageManifest {
    std::string id;
    std::string version{"0.1.0"};
    std::string engine_api{">=0.1.0,<0.2.0"};
    std::string entry_point{"scripts.game"};
    std::string board_file{"boards/primary.board.toml"};
    std::optional<std::string> visuals;
    std::uint32_t save_compatibility{1U};
    std::vector<std::string> assets;
    std::vector<std::string> permissions;
    std::vector<std::string> dependencies;
    std::vector<std::string> native_extensions;
    bool native_enabled_by_default{false};

    auto operator<=>(const PackageManifest&) const = default;
};

struct BoardEntity {
    BoardEntity() = default;
    BoardEntity(std::string entity_name, std::string entity_type,
                std::uint32_t entity_owner, std::uint32_t entity_x,
                std::uint32_t entity_y, std::uint32_t entity_sprite,
                std::string named_sprite = {})
        : name(std::move(entity_name)), type(std::move(entity_type)), owner(entity_owner),
          x(entity_x), y(entity_y), sprite(entity_sprite),
          sprite_name(std::move(named_sprite)) {}

    std::string name;
    std::string type;
    std::uint32_t owner{0U};
    std::uint32_t x{0U};
    std::uint32_t y{0U};
    std::uint32_t sprite{0U};
    std::string sprite_name;

    auto operator<=>(const BoardEntity&) const = default;
};

struct BoardDefinition {
    static constexpr std::uint32_t maximum_extent = 256U;
    static constexpr std::uint32_t maximum_entities = 65'536U;

    std::uint32_t width{8U};
    std::uint32_t height{8U};
    std::uint32_t side_to_move{0U};
    std::uint32_t castling_rights{15U};
    std::vector<BoardEntity> entities;

    auto operator<=>(const BoardDefinition&) const = default;
};

class PackageDocument {
  public:
    static constexpr std::uintmax_t maximum_text_file_size = 1U << 20U;

    [[nodiscard]] static std::expected<PackageDocument, Diagnostic>
    create(const std::filesystem::path& root, std::string package_id,
           std::uint32_t width = 8U, std::uint32_t height = 8U);
    [[nodiscard]] static std::expected<PackageDocument, Diagnostic>
    open(const std::filesystem::path& root);

    [[nodiscard]] std::expected<void, Diagnostic> save() const;
    [[nodiscard]] std::vector<Diagnostic> validate() const;
    [[nodiscard]] std::expected<Topology, Diagnostic> topology() const;
    [[nodiscard]] std::expected<RenderSnapshot, Diagnostic>
    preview_snapshot(std::uint64_t revision) const;
    /// Validate and import a PNG into assets/, update the allowlist and theme, and
    /// atomically save the package. Existing files are never overwritten.
    [[nodiscard]] std::expected<std::filesystem::path, Diagnostic>
    import_png(const std::filesystem::path& source, std::string sprite_key);
    [[nodiscard]] std::expected<VisualTheme, Diagnostic> visual_theme() const;

    [[nodiscard]] std::expected<void, Diagnostic>
    set_manifest(PackageManifest manifest);
    void set_python_source(std::string source) { python_source_ = std::move(source); }
    [[nodiscard]] std::expected<void, Diagnostic>
    regenerate_board(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] std::expected<void, Diagnostic> reset_chess_setup();
    [[nodiscard]] std::expected<void, Diagnostic>
    upsert_entity(BoardEntity entity, std::string_view replaced_name = {});
    [[nodiscard]] bool remove_entity(std::string_view name);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const PackageManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const BoardDefinition& board() const noexcept { return board_; }
    [[nodiscard]] const std::string& python_source() const noexcept { return python_source_; }
    [[nodiscard]] std::filesystem::path manifest_path() const;
    [[nodiscard]] std::filesystem::path board_path() const;
    [[nodiscard]] std::filesystem::path python_path() const;
    [[nodiscard]] const BoardEntity* entity_at(std::uint32_t x,
                                               std::uint32_t y) const noexcept;

  private:
    PackageDocument(std::filesystem::path root, PackageManifest manifest,
                    BoardDefinition board, std::string python_source)
        : root_(std::move(root)), manifest_(std::move(manifest)),
          board_(std::move(board)), python_source_(std::move(python_source)) {}

    std::filesystem::path root_;
    PackageManifest manifest_;
    BoardDefinition board_;
    std::string python_source_;
};

[[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);

} // namespace ludus::studio
