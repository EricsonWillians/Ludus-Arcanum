#pragma once

#include "ludus/core/id.hpp"
#include "ludus/render/math.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ludus {

struct SpriteId {
    std::uint32_t value{0U};

    auto operator<=>(const SpriteId&) const = default;
};

enum class SpaceShape : std::uint8_t {
    rectangle,
    rounded_rectangle,
    circle,
    hexagon,
};

enum class ActionVisualKind : std::uint8_t {
    move,
    capture,
    castle,
    promotion,
    draw_claim,
    attack,
    ability,
    choice,
};

enum class ActionTargetSemantics : std::uint8_t {
    space,
    entity,
    self,
    area,
    direction,
};

enum class EffectKind : std::uint8_t {
    glow,
    pulse,
    projectile,
    impact,
    poison,
    capture_fade,
    scale,
    check,
    promotion,
    camera_shake,
};

enum class EffectBlend : std::uint8_t { alpha, additive };

struct SpaceVisual {
    SpaceId id;
    Rect bounds;
    Color color;
    SpaceShape shape{SpaceShape::rectangle};
    Color border{0.0F, 0.0F, 0.0F, 0.0F};
    float border_width{0.0F};
    float rotation{0.0F};

    auto operator<=>(const SpaceVisual&) const = default;
};

struct LinkVisual {
    SpaceId from;
    SpaceId to;
    Vec2 from_center;
    Vec2 to_center;
    Color color{0.35F, 0.7F, 0.95F, 0.7F};
    float width{0.06F};

    auto operator<=>(const LinkVisual&) const = default;
};

/// Backend-neutral free-standing geometry, separate from pickable board spaces.
struct ShapeVisual {
    Rect bounds;
    Color color;
    SpaceShape shape{SpaceShape::rectangle};
    Color border{0.0F, 0.0F, 0.0F, 0.0F};
    float border_width{0.0F};
    float rotation{0.0F};
    float layer{0.0F};

    auto operator<=>(const ShapeVisual&) const = default;
};

struct PieceVisual {
    EntityId id;
    SpaceId location;
    Vec2 center;
    Vec2 size{0.8F, 0.8F};
    SpriteId sprite;
    Color tint;
    float layer{0.0F};
    float rotation{0.0F};
    Color outline{0.0F, 0.0F, 0.0F, 0.0F};
    float outline_width{0.0F};

    auto operator<=>(const PieceVisual&) const = default;
};

/// Package-native ornament or material sprite that is not an authoritative entity.
struct DecorationSprite {
    SpriteId sprite;
    Vec2 center;
    Vec2 size{1.0F, 1.0F};
    Color tint{1.0F, 1.0F, 1.0F, 1.0F};
    float layer{0.0F};
    float rotation{0.0F};

    auto operator<=>(const DecorationSprite&) const = default;
};

enum class InteractionTargetKind : std::uint8_t {
    quiet_move,
    capture,
    castle,
    promotion,
    draw_claim,
    hover,
    keyboard_focus,
    selection,
    invalid,
};

struct InteractionTarget {
    SpaceId space;
    InteractionTargetKind kind{InteractionTargetKind::quiet_move};

    auto operator<=>(const InteractionTarget&) const = default;
};

struct DragInteraction {
    EntityId entity;
    SpaceId origin;
    Vec2 pointer;
    bool active{false};
    bool valid{false};

    auto operator<=>(const DragInteraction&) const = default;
};

/// Value-only transient canvas state. It never participates in saves or hashes.
struct InteractionState {
    std::optional<EntityId> selected;
    std::optional<SpaceId> hovered;
    std::optional<SpaceId> keyboard_focus;
    std::vector<InteractionTarget> targets;
    std::optional<DragInteraction> drag;
    bool reduced_motion{false};

    auto operator<=>(const InteractionState&) const = default;
};

/// A package-defined action token paired with generic presentation handles.
struct ActionHint {
    std::uint64_t token{0U};
    EntityId actor;
    SpaceId origin;
    SpaceId destination;
    std::uint32_t variant{0U};
    ActionVisualKind kind{ActionVisualKind::move};
    std::string label;
    std::optional<SpriteId> icon;
    ActionTargetSemantics target{ActionTargetSemantics::space};

    auto operator<=>(const ActionHint&) const = default;
};

/// A package-defined decision token with presentation-only text.
struct ChoiceHint {
    std::uint64_t token{0U};
    std::string label;

    auto operator<=>(const ChoiceHint&) const = default;
};

struct PieceAnimation {
    EntityId entity;
    Vec2 from;
    Vec2 to;
    std::chrono::steady_clock::time_point start;
    std::chrono::milliseconds duration{180};
    bool loop{false};

    auto operator<=>(const PieceAnimation&) const = default;
};

struct BarVisual {
    Rect bounds;
    float value{0.0F};
    float maximum{1.0F};
    Color background{0.08F, 0.08F, 0.1F, 0.9F};
    Color fill{0.28F, 0.78F, 0.4F, 1.0F};
    float layer{10.0F};

    auto operator<=>(const BarVisual&) const = default;
};

struct TextVisual {
    std::string text;
    Vec2 position;
    float size{14.0F};
    Color color;
    float layer{20.0F};
    bool screen_space{false};

    auto operator<=>(const TextVisual&) const = default;
};

struct EffectVisual {
    EffectKind kind{EffectKind::glow};
    Vec2 from;
    Vec2 to;
    Color color;
    float size{0.4F};
    std::chrono::steady_clock::time_point start;
    std::chrono::milliseconds duration{300};
    float layer{15.0F};
    EffectBlend blend{EffectBlend::alpha};
    float initial_scale{1.0F};
    float final_scale{1.0F};

    auto operator<=>(const EffectVisual&) const = default;
};

/// Immutable after publication. Rendering receives shared_ptr<const RenderSnapshot>.
struct RenderSnapshot {
    std::uint64_t revision{0U};
    std::uint64_t static_revision{0U};
    std::uint64_t dynamic_revision{0U};
    Rect world_bounds;
    std::vector<SpaceVisual> spaces;
    std::vector<LinkVisual> links;
    std::vector<ShapeVisual> shapes;
    std::vector<DecorationSprite> decorations;
    std::vector<PieceVisual> pieces;
    std::vector<ActionHint> actions;
    std::vector<ChoiceHint> choices;
    std::vector<PieceAnimation> animations;
    std::vector<BarVisual> bars;
    std::vector<TextVisual> texts;
    std::vector<EffectVisual> effects;
    std::string status;
};

[[nodiscard]] std::optional<SpaceId> pick_space(const RenderSnapshot& snapshot,
                                                Vec2 world_point) noexcept;
[[nodiscard]] const PieceVisual* find_piece(const RenderSnapshot& snapshot,
                                            EntityId entity) noexcept;
[[nodiscard]] const PieceVisual* find_piece_at(const RenderSnapshot& snapshot,
                                               SpaceId space) noexcept;
[[nodiscard]] Vec2 animated_center(const RenderSnapshot& snapshot, const PieceVisual& piece,
                                   std::chrono::steady_clock::time_point now) noexcept;
[[nodiscard]] bool has_active_animations(const RenderSnapshot& snapshot,
                                         std::chrono::steady_clock::time_point now) noexcept;

} // namespace ludus
