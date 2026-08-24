#include "ludus/studio/package_document.hpp"
#include "ludus/studio/inspection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ludus-studio-test-" + std::to_string(nonce));
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("studio packages create, save, reopen, and preview without GTK",
          "[studio][package][roundtrip]") {
    TemporaryDirectory temporary;
    auto document = ludus::studio::PackageDocument::create(
        temporary.path(), "org.example.first-variation");
    REQUIRE(document);
    REQUIRE(std::filesystem::exists(document->manifest_path()));
    REQUIRE(std::filesystem::exists(document->board_path()));
    REQUIRE(std::filesystem::exists(document->python_path()));
    REQUIRE(document->board().entities.size() == 32U);

    auto preview = document->preview_snapshot(7U);
    REQUIRE(preview);
    REQUIRE(preview->revision == 7U);
    REQUIRE(preview->spaces.size() == 64U);
    REQUIRE(preview->links.size() == 112U);
    REQUIRE(preview->pieces.size() == 32U);

    const auto source = document->python_source() + "\n# saved from the studio\n";
    document->set_python_source(source);
    REQUIRE(document->save());
    const auto reopened = ludus::studio::PackageDocument::open(temporary.path());
    REQUIRE(reopened);
    REQUIRE(reopened->manifest() == document->manifest());
    REQUIRE(reopened->board() == document->board());
    REQUIRE(reopened->python_source() == source);
    REQUIRE_FALSE(ludus::studio::PackageDocument::create(
        temporary.path(), "org.example.would-overwrite"));
}

TEST_CASE("board generation and entity edits remain validated and deterministic",
          "[studio][board][entity]") {
    TemporaryDirectory temporary;
    auto document = ludus::studio::PackageDocument::create(
        temporary.path(), "org.example.board-edit");
    REQUIRE(document);
    REQUIRE(document->regenerate_board(10U, 6U));
    REQUIRE(document->board().width == 10U);
    REQUIRE(document->board().height == 6U);
    REQUIRE(document->board().entities.size() == 16U);
    REQUIRE(document->board().castling_rights == 0U);

    REQUIRE(document->upsert_entity(
        {"white_custom", "knight", 0U, 8U, 5U, 1U}));
    REQUIRE(document->entity_at(8U, 5U) != nullptr);
    REQUIRE(document->upsert_entity(
        {"white_renamed", "knight", 0U, 8U, 5U, 1U}, "white_custom"));
    REQUIRE(document->entity_at(8U, 5U)->name == "white_renamed");
    REQUIRE_FALSE(document->upsert_entity(
        {"invalid_rename", "knight", 0U, 99U, 99U, 1U}, "white_renamed"));
    REQUIRE(document->entity_at(8U, 5U)->name == "white_renamed");
    const auto collision = document->upsert_entity(
        {"black_custom", "bishop", 1U, 8U, 5U, 2U});
    REQUIRE_FALSE(collision);
    REQUIRE(collision.error().message.find("occupy") != std::string::npos);
    REQUIRE(document->remove_entity("white_renamed"));
    REQUIRE(document->entity_at(8U, 5U) == nullptr);

    const auto topology = document->topology();
    REQUIRE(topology);
    REQUIRE(topology->spaces().size() == 60U);
    REQUIRE(topology->links().size() == 208U);
}

TEST_CASE("studio package diagnostics identify malformed source files",
          "[studio][package][diagnostics]") {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path());
    const auto manifest = temporary.path() / "game.toml";
    {
        std::ofstream output{manifest};
        REQUIRE(output);
        output << "[package]\n"
                  "id = \"org.example.bad\"\n"
                  "version = [not, supported]\n";
    }
    const auto opened = ludus::studio::PackageDocument::open(temporary.path());
    REQUIRE_FALSE(opened);
    REQUIRE(opened.error().source.path == manifest.string());
    REQUIRE(opened.error().source.line == 3U);
}

TEST_CASE("studio package validation rejects paths outside the package",
          "[studio][package][security]") {
    TemporaryDirectory temporary;
    auto document = ludus::studio::PackageDocument::create(
        temporary.path(), "org.example.safe-paths");
    REQUIRE(document);
    auto manifest = document->manifest();
    manifest.board_file = "../outside.toml";
    const auto updated = document->set_manifest(std::move(manifest));
    REQUIRE_FALSE(updated);
    REQUIRE(updated.error().message.find("inside the package") != std::string::npos);
    REQUIRE(document->manifest().board_file == "boards/primary.board.toml");
}

TEST_CASE("Studio imports validated PNGs with collision-safe theme updates and rollback",
          "[studio][assets][theme][reload]") {
    TemporaryDirectory temporary;
    auto document = ludus::studio::PackageDocument::create(
        temporary.path(), "org.example.visual-assets");
    REQUIRE(document);
    const auto source = std::filesystem::path{LUDUS_SOURCE_DIR} /
                        "games/tactical_rpg/assets/cards/focus.png";
    const auto first = document->import_png(source, "card.focus");
    REQUIRE(first);
    REQUIRE(*first == std::filesystem::path{"assets/focus.png"});
    const auto second = document->import_png(source, "card.focus_alt");
    REQUIRE(second);
    REQUIRE(*second == std::filesystem::path{"assets/focus-2.png"});
    REQUIRE(document->manifest().assets.size() == 2U);

    const auto theme = document->visual_theme();
    REQUIRE(theme);
    REQUIRE(theme->sprite("card.focus") == ludus::SpriteId{0U});
    REQUIRE(theme->sprite("card.focus_alt") == ludus::SpriteId{1U});
    auto catalog = ludus::AssetCatalog::load_package(temporary.path(), "game.toml");
    REQUIRE(catalog);
    const auto generation = catalog->generation();
    {
        std::ofstream broken{temporary.path() / "visuals/theme.toml",
                             std::ios::trunc};
        REQUIRE(broken);
        broken << "[theme]\nid = \"broken\"\n";
    }
    REQUIRE_FALSE(catalog->reload());
    REQUIRE(catalog->generation() == generation);
    REQUIRE(catalog->sprite("card.focus") == ludus::SpriteId{0U});
    REQUIRE(catalog->last_diagnostic());
}

TEST_CASE("Studio upgrades legacy numeric sprites to stable names on save",
          "[studio][assets][legacy]") {
    TemporaryDirectory temporary;
    auto document = ludus::studio::PackageDocument::create(
        temporary.path(), "org.example.legacy-sprites");
    REQUIRE(document);
    {
        std::ofstream board{document->board_path(), std::ios::trunc};
        REQUIRE(board);
        board << "[board]\n"
                 "kind = \"rectangular\"\n"
                 "width = 8\n"
                 "height = 8\n"
                 "side_to_move = 0\n"
                 "castling_rights = 0\n\n"
                 "[[entity]]\n"
                 "name = \"known\"\n"
                 "type = \"pawn\"\n"
                 "owner = 0\n"
                 "x = 0\n"
                 "y = 0\n"
                 "sprite = 0\n\n"
                 "[[entity]]\n"
                 "name = \"unknown\"\n"
                 "type = \"relic\"\n"
                 "owner = 0\n"
                 "x = 1\n"
                 "y = 0\n"
                 "sprite = 37\n";
    }

    auto reopened = ludus::studio::PackageDocument::open(temporary.path());
    REQUIRE(reopened);
    REQUIRE(reopened->board().entities[0].sprite_name == "piece.ivory.pawn");
    REQUIRE(reopened->board().entities[1].sprite_name == "legacy.unknown.sprite_37");
    REQUIRE(reopened->save());

    std::ifstream saved{reopened->board_path()};
    REQUIRE(saved);
    std::ostringstream buffer;
    buffer << saved.rdbuf();
    const auto contents = buffer.str();
    REQUIRE(contents.find("sprite = ") == std::string::npos);
    REQUIRE(contents.find("sprite_name = \"piece.ivory.pawn\"") != std::string::npos);
    REQUIRE(contents.find("sprite_name = \"legacy.unknown.sprite_37\"") !=
            std::string::npos);
}

TEST_CASE("state and event inspectors expose stable value data", "[studio][inspection]") {
    ludus::SymbolRegistry symbols;
    const auto property = symbols.properties.intern("energy");
    ludus::TopologyBuilder builder;
    const auto space = builder.add_space();
    auto topology = std::move(builder).build();
    REQUIRE(topology);
    const ludus::GameState state{std::move(symbols), std::move(*topology)};

    const ludus::EventBatch batch{
        {{1U, ludus::EntityPropertyChanged{ludus::EntityId{3U, 1U}, property,
                                           ludus::PropertyValue{std::int64_t{1}},
                                           ludus::PropertyValue{std::int64_t{2}}}},
         {2U, ludus::EntityMoved{ludus::EntityId{3U, 1U}, std::nullopt, space}}},
        0x1234U};
    const std::array batches{batch};
    const auto events = ludus::studio::inspect_event_log(batches, 1U, state.symbols());
    REQUIRE(events.find("[applied] batch 1") != std::string::npos);
    REQUIRE(events.find("property=energy") != std::string::npos);
    REQUIRE(events.find("EntityMoved") != std::string::npos);
    const auto summary = ludus::studio::inspect_state(state);
    REQUIRE(summary.find("Spaces: 1") != std::string::npos);
    REQUIRE(summary.find("Entities: 0") != std::string::npos);
}
