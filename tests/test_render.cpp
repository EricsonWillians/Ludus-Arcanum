#include "ludus/render/animation.hpp"
#include "ludus/render/atlas.hpp"
#include "ludus/render/batch.hpp"
#include "ludus/render/camera.hpp"
#include "ludus/render/exchange.hpp"
#include "ludus/render/snapshot.hpp"
#include "ludus/render/theme.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ludus-render-test-" + std::to_string(nonce));
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

ludus::ImageRgba solid_image(std::uint32_t width, std::uint32_t height,
                             std::uint8_t value) {
    return {width, height,
            std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U, value)};
}

} // namespace

TEST_CASE("camera fitting, resizing, zoom anchoring, and coordinate conversion agree",
          "[render][camera]") {
    ludus::Camera2D camera;
    camera.resize(1'600, 900);
    camera.fit({{-4.0F, -4.0F}, {4.0F, 4.0F}}, 1.0F);

    const ludus::Vec2 center_screen{800.0F, 450.0F};
    REQUIRE(camera.screen_to_world(center_screen).x == Catch::Approx(0.0F));
    REQUIRE(camera.screen_to_world(center_screen).y == Catch::Approx(0.0F));

    camera.resize(450, 900);
    REQUIRE(camera.visible_half_width() == Catch::Approx(4.0F));
    REQUIRE(camera.visible_half_height() == Catch::Approx(8.0F));
    camera.resize(1'600, 900);

    const ludus::Vec2 anchor{1'200.0F, 300.0F};
    const auto before = camera.screen_to_world(anchor);
    camera.zoom_at(anchor, 2.0F);
    const auto after = camera.screen_to_world(anchor);
    REQUIRE(after.x == Catch::Approx(before.x));
    REQUIRE(after.y == Catch::Approx(before.y));
    REQUIRE(camera.zoom() == Catch::Approx(2.0F));

    camera.pan_pixels({100.0F, -50.0F});
    camera.rotate_at(anchor, 0.45F);
    const auto round_trip = camera.world_to_screen(camera.screen_to_world(anchor));
    REQUIRE(round_trip.x == Catch::Approx(anchor.x));
    REQUIRE(round_trip.y == Catch::Approx(anchor.y));
}

TEST_CASE("PNG package textures retain alpha and resolve through named visual themes",
          "[render][atlas][theme]") {
    const auto package = std::filesystem::path{LUDUS_SOURCE_DIR} / "games/tactical_rpg";
    const auto image =
        ludus::load_png_rgba(package / "assets/units/vanguard-ranger.png");
    REQUIRE(image);
    REQUIRE(image->width == 512U);
    REQUIRE(image->height == 704U);
    REQUIRE(image->valid());
    bool has_transparency = false;
    for (std::size_t index = 3U; index < image->pixels.size(); index += 4U) {
        has_transparency = has_transparency || image->pixels[index] == 0U;
    }
    REQUIRE(has_transparency);

    const auto theme = ludus::VisualTheme::load_package(package);
    INFO((theme ? std::string{} : theme.error().message));
    REQUIRE(theme);
    REQUIRE(theme->id() == "dark-fantasy");
    REQUIRE(theme->sprite("unit.ranger") == ludus::SpriteId{0U});
    REQUIRE(theme->sprite("card.focus") == ludus::SpriteId{7U});
    REQUIRE(theme->sprite("terrain.ruin") == ludus::SpriteId{8U});
    REQUIRE_FALSE(theme->sprite("missing"));
    REQUIRE(theme->atlas().image().valid());
}

TEST_CASE("shape-aware picking rejects points outside a hexagon", "[render][picking]") {
    ludus::RenderSnapshot snapshot;
    snapshot.spaces.push_back(ludus::SpaceVisual{
        ludus::SpaceId{7U, 1U}, {{-1.0F, -1.0F}, {1.0F, 1.0F}}, {},
        ludus::SpaceShape::hexagon});
    REQUIRE(ludus::pick_space(snapshot, {0.0F, 0.0F}) == ludus::SpaceId{7U, 1U});
    REQUIRE_FALSE(ludus::pick_space(snapshot, {0.95F, 0.95F}));
}

TEST_CASE("visual themes crop sprite-sheet regions and retain font preferences",
          "[render][atlas][theme][sheet]") {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path() / "assets");
    std::filesystem::create_directories(temporary.path() / "visuals");
    const auto source = std::filesystem::path{LUDUS_SOURCE_DIR} /
                        "games/tactical_rpg/assets/cards/focus.png";
    REQUIRE(std::filesystem::copy_file(source, temporary.path() / "assets/sheet.png"));
    {
        std::ofstream theme{temporary.path() / "visuals/theme.toml"};
        REQUIRE(theme);
        theme << "[theme]\n"
                 "id = \"sheet-test\"\n"
                 "font_families = [\"Cinzel\", \"Serif\"]\n\n"
                 "[[sprite]]\n"
                 "id = \"card.left\"\n"
                 "source = \"assets/sheet.png\"\n"
                 "region_x = 0\n"
                 "region_y = 0\n"
                 "region_width = 192\n"
                 "region_height = 256\n"
                 "filter = \"nearest\"\n\n"
                 "[[sprite]]\n"
                 "id = \"card.right\"\n"
                 "source = \"assets/sheet.png\"\n"
                 "region_x = 192\n"
                 "region_y = 256\n"
                 "region_width = 192\n"
                 "region_height = 256\n";
    }
    const std::vector<std::string> assets{"assets/sheet.png"};
    const auto theme = ludus::VisualTheme::load(
        temporary.path(), "visuals/theme.toml", assets);
    INFO((theme ? std::string{} : theme.error().message));
    REQUIRE(theme);
    REQUIRE(theme->font_families().size() == 2U);
    REQUIRE(theme->font_family() == "Cinzel");
    REQUIRE(theme->sprites()[0].source_region ==
            ludus::SpriteSheetRegion{0U, 0U, 192U, 256U});
    REQUIRE(theme->atlas().region(ludus::SpriteId{0U})->width == 192U);
    REQUIRE(theme->atlas().region(ludus::SpriteId{0U})->nearest);
    REQUIRE(theme->atlas().region(ludus::SpriteId{1U})->height == 256U);
}

TEST_CASE("texture images pack into a bounded atlas with stable numeric regions",
          "[render][atlas]") {
    const std::vector<ludus::ImageRgba> images{
        solid_image(3U, 5U, 32U),
        solid_image(7U, 2U, 128U),
        solid_image(4U, 4U, 255U),
    };
    const auto atlas = ludus::TextureAtlas::pack(images, 1U);
    REQUIRE(atlas);
    REQUIRE(atlas->image().valid());
    REQUIRE(atlas->regions().size() == images.size());
    for (std::uint32_t index = 0U; index < images.size(); ++index) {
        const auto* region = atlas->region(ludus::SpriteId{index});
        REQUIRE(region != nullptr);
        REQUIRE(region->width == images[index].width);
        REQUIRE(region->height == images[index].height);
        REQUIRE(region->u_min < region->u_max);
        REQUIRE(region->v_min < region->v_max);
    }
    REQUIRE(atlas->region(ludus::SpriteId{99U}) == nullptr);
}

TEST_CASE("atlas packing spills deterministically across bounded extruded pages",
          "[render][atlas][multipage]") {
    const std::vector<ludus::ImageRgba> images{
        solid_image(40U, 40U, 32U), solid_image(40U, 40U, 96U),
        solid_image(40U, 40U, 192U),
    };
    const auto atlas = ludus::TextureAtlas::pack(images, 1U, 64U);
    REQUIRE(atlas);
    REQUIRE(atlas->pages().size() == 3U);
    REQUIRE(atlas->region(ludus::SpriteId{0U})->page == 0U);
    REQUIRE(atlas->region(ludus::SpriteId{1U})->page == 1U);
    REQUIRE(atlas->region(ludus::SpriteId{2U})->page == 2U);
    for (const auto& page : atlas->pages()) {
        REQUIRE(page.width == 64U);
        REQUIRE(page.height == 64U);
    }
    const auto* region = atlas->region(ludus::SpriteId{0U});
    REQUIRE(region != nullptr);
    const auto& page = atlas->pages()[region->page];
    const auto edge = (static_cast<std::size_t>(region->y) * page.width + region->x) * 4U;
    const auto extrusion =
        (static_cast<std::size_t>(region->y - 1U) * page.width + region->x - 1U) * 4U;
    REQUIRE(page.pixels[edge + 3U] == 32U);
    REQUIRE(page.pixels[extrusion + 3U] == 32U);
}

TEST_CASE("PPM textures load into normalized RGBA pixels", "[render][atlas][io]") {
    const auto path = std::filesystem::temp_directory_path() / "ludus-render-test.ppm";
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        REQUIRE(output);
        output << "P3\n# deterministic fixture\n2 1\n15\n15 0 0  0 8 15\n";
    }
    const auto image = ludus::load_ppm_rgba(path);
    std::filesystem::remove(path);
    REQUIRE(image);
    REQUIRE(image->width == 2U);
    REQUIRE(image->height == 1U);
    REQUIRE(image->pixels ==
            std::vector<std::uint8_t>{255U, 0U, 0U, 255U, 0U, 136U, 255U, 255U});
}

TEST_CASE("snapshot picking and event animation use only immutable presentation data",
          "[render][snapshot][animation]") {
    const ludus::EntityId entity{3U, 1U};
    const ludus::SpaceId from{0U, 1U};
    const ludus::SpaceId to{1U, 1U};
    ludus::RenderSnapshot before;
    before.spaces = {
        {from, {{-1.0F, -0.5F}, {0.0F, 0.5F}}, {}},
        {to, {{0.0F, -0.5F}, {1.0F, 0.5F}}, {}},
    };
    before.pieces = {{entity, from, {-0.5F, 0.0F}, {}, {}, {}, 0.0F}};
    auto after = before;
    after.pieces.front().location = to;
    after.pieces.front().center = {0.5F, 0.0F};
    const auto start = std::chrono::steady_clock::time_point{};
    const ludus::EventBatch batch{{ludus::Event{
                                       1U, ludus::EntityMoved{entity, from, to}}},
                                   0U};
    const ludus::EventAnimationAdapter adapter{std::chrono::milliseconds{200}};
    after.animations = adapter.adapt(batch, before, after, start);

    REQUIRE(ludus::pick_space(after, {-0.25F, 0.0F}) == from);
    REQUIRE(ludus::pick_space(after, {0.25F, 0.0F}) == to);
    REQUIRE(ludus::find_piece(after, entity) != nullptr);
    REQUIRE(ludus::find_piece_at(after, to) != nullptr);
    REQUIRE(after.animations.size() == 1U);
    const auto midpoint = ludus::animated_center(
        after, after.pieces.front(), start + std::chrono::milliseconds{100});
    REQUIRE(midpoint.x == Catch::Approx(0.0F));
    REQUIRE(midpoint.y == Catch::Approx(0.0F));
    REQUIRE(ludus::has_active_animations(
        after, start + std::chrono::milliseconds{199}));
    REQUIRE_FALSE(ludus::has_active_animations(
        after, start + std::chrono::milliseconds{200}));
}

TEST_CASE("sprite batching preserves layer order and reuses warmed storage",
          "[render][batch]") {
    ludus::RenderSnapshot snapshot;
    const ludus::SpaceId space{0U, 1U};
    const ludus::EntityId foreground{1U, 1U};
    const ludus::EntityId background{2U, 1U};
    snapshot.spaces = {{space, {{-0.5F, -0.5F}, {0.5F, 0.5F}}, {}}};
    snapshot.links = {{space, space, {-1.0F, -1.0F}, {1.0F, 1.0F}, {}, 0.1F}};
    snapshot.pieces = {
        {foreground, space, {-1.0F, 0.0F}, {}, {}, {}, 2.0F},
        {background, space, {1.0F, 0.0F}, {}, {}, {}, 1.0F},
    };

    ludus::SpriteBatch batch;
    const std::vector legal_destinations{space};
    batch.prepare(snapshot, nullptr, std::nullopt, foreground, legal_destinations,
                  std::chrono::steady_clock::time_point{});
    const auto first = batch.instances();
    REQUIRE(first.size() == 6U);
    REQUIRE(first[1U].rotation == Catch::Approx(0.785398F));
    REQUIRE(first[2U].center[0] == Catch::Approx(1.0F));
    REQUIRE(first[3U].center[0] == Catch::Approx(-1.0F));
    REQUIRE(first[3U].layer == Catch::Approx(2.0F));
    REQUIRE(first.back().layer == Catch::Approx(3.0F));
    const auto warmed_capacity = batch.capacity();
    const auto static_builds = batch.static_build_count();

    batch.prepare(snapshot, nullptr, std::nullopt, std::nullopt, {},
                  std::chrono::steady_clock::time_point{});
    REQUIRE(batch.instances().size() == 4U);
    REQUIRE(batch.capacity() == warmed_capacity);
    REQUIRE(batch.static_build_count() == static_builds);
}

TEST_CASE("effect batches preserve alpha ordering before additive effects",
          "[render][batch][effects]") {
    ludus::RenderSnapshot snapshot;
    snapshot.spaces.push_back({ludus::SpaceId{0U, 1U},
                               {{-1.0F, -1.0F}, {1.0F, 1.0F}}, {}});
    const auto start = std::chrono::steady_clock::time_point{};
    snapshot.effects.push_back(ludus::EffectVisual{
        ludus::EffectKind::impact, {}, {0.0F, 0.0F}, {1.0F, 0.2F, 0.1F, 0.8F},
        0.25F, start, std::chrono::milliseconds{1'000}, 15.0F,
        ludus::EffectBlend::alpha, 0.5F, 1.5F});
    snapshot.effects.push_back(ludus::EffectVisual{
        ludus::EffectKind::check, {}, {1.0F, 0.0F}, {1.0F, 0.1F, 0.1F, 0.6F},
        0.3F, start, std::chrono::milliseconds{1'000}, 16.0F,
        ludus::EffectBlend::additive, 0.8F, 1.2F});

    ludus::SpriteBatch batch;
    batch.prepare(snapshot, nullptr, std::nullopt, std::nullopt, {},
                  start + std::chrono::milliseconds{500});
    REQUIRE(batch.instances().size() == 3U);
    REQUIRE(batch.additive_offset() == 2U);
    REQUIRE(batch.instances()[1U].center[0] == Catch::Approx(0.0F));
    REQUIRE(batch.instances()[2U].center[0] == Catch::Approx(1.0F));
}

TEST_CASE("typed interaction targets and decorations batch without authoritative state",
          "[render][batch][interaction]") {
    ludus::RenderSnapshot snapshot;
    const ludus::SpaceId origin{0U, 1U};
    const ludus::SpaceId destination{1U, 1U};
    const ludus::EntityId entity{3U, 1U};
    snapshot.spaces = {
        {origin, {{-1.0F, -0.5F}, {0.0F, 0.5F}}, {}},
        {destination, {{0.0F, -0.5F}, {1.0F, 0.5F}}, {}},
    };
    snapshot.decorations.push_back(
        ludus::DecorationSprite{ludus::SpriteId{9U}, {0.0F, 0.0F}, {2.0F, 1.0F},
                                {1.0F, 1.0F, 1.0F, 0.2F}, 0.1F});
    snapshot.pieces.push_back(
        ludus::PieceVisual{entity, origin, {-0.5F, 0.0F}, {0.8F, 0.8F}, {}, {}, 1.0F});
    ludus::InteractionState interaction;
    interaction.selected = entity;
    interaction.keyboard_focus = destination;
    interaction.targets = {
        {destination, ludus::InteractionTargetKind::capture},
        {destination, ludus::InteractionTargetKind::promotion},
    };
    interaction.drag = ludus::DragInteraction{entity, origin, {0.25F, 0.1F}, true, true};

    ludus::SpriteBatch batch;
    batch.prepare(snapshot, nullptr, interaction,
                  std::chrono::steady_clock::time_point{});
    REQUIRE(batch.instances().size() == 8U);
    REQUIRE(std::ranges::none_of(batch.instances(), [](const auto& instance) {
        return instance.layer == Catch::Approx(1.0F) &&
               instance.center[0] == Catch::Approx(-0.5F);
    }));
    REQUIRE(std::ranges::any_of(batch.instances(), [](const auto& instance) {
        return instance.layer == Catch::Approx(50.0F) &&
               instance.center[0] == Catch::Approx(0.25F);
    }));
}

TEST_CASE("snapshot exchange publishes an immutable latest value without a render lock",
          "[render][snapshot][threading]") {
    ludus::RenderSnapshotExchange exchange;
    ludus::RenderSnapshot first_value;
    first_value.revision = 1U;
    auto first = exchange.publish(std::move(first_value));
    REQUIRE(first->revision == 1U);

    std::jthread producer{[&exchange] {
        ludus::RenderSnapshot second_value;
        second_value.revision = 2U;
        static_cast<void>(exchange.publish(std::move(second_value)));
    }};
    producer.join();
    const auto latest = exchange.load();
    REQUIRE(latest);
    REQUIRE(latest->revision == 2U);
    REQUIRE(first->revision == 1U);
}

TEST_CASE("player views publish board and value-only HUD records atomically",
          "[render][exchange][hud]") {
    ludus::PlayerViewExchange exchange;
    ludus::PlayerView view;
    view.render.revision = 9U;
    view.units.push_back(ludus::UnitCardView{ludus::EntityId{4U, 1U}, "Ranger",
                                             "Vanguard", 8, 12, 2, std::nullopt,
                                             {"Poisoned"}});
    view.objective = ludus::ObjectiveScoreView{"Vanguard", "Raiders", 1, 2, 3};
    const auto published = exchange.publish(std::move(view));
    const auto loaded = exchange.load();
    REQUIRE(loaded == published);
    REQUIRE(loaded->render.revision == 9U);
    REQUIRE(loaded->units.front().statuses == std::vector<std::string>{"Poisoned"});
    REQUIRE(loaded->objective->target == 3);
}
