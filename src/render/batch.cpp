#include "ludus/render/batch.hpp"

#include <algorithm>
#include <cmath>

namespace ludus {
namespace {

QuadInstance solid_instance(Rect bounds, Color color,
                            SpaceShape shape = SpaceShape::rectangle,
                            float rotation = 0.0F, float border_width = 0.0F,
                            Color border = {}, float layer = 0.0F) {
    return QuadInstance{{bounds.center().x, bounds.center().y},
                        {bounds.width(), bounds.height()},
                        {color.red, color.green, color.blue, color.alpha},
                        {0.0F, 0.0F, 1.0F, 1.0F},
                        0.0F,
                        rotation,
                        static_cast<float>(shape),
                        border_width,
                        {border.red, border.green, border.blue, border.alpha},
                        0.0F,
                        0.0F,
                        layer};
}

QuadInstance link_instance(const LinkVisual& link) {
    const auto delta = link.to_center - link.from_center;
    const auto length = std::hypot(delta.x, delta.y);
    const auto center = (link.from_center + link.to_center) * 0.5F;
    return QuadInstance{{center.x, center.y},
                        {length, std::max(link.width, 0.0F)},
                        {link.color.red, link.color.green, link.color.blue,
                         link.color.alpha},
                        {0.0F, 0.0F, 1.0F, 1.0F},
                        0.0F,
                        std::atan2(delta.y, delta.x),
                        static_cast<float>(SpaceShape::rounded_rectangle),
                        0.0F,
                        {0.0F, 0.0F, 0.0F, 0.0F},
                        0.0F,
                        0.0F,
                        0.0F};
}

const SpaceVisual* visual_space(const RenderSnapshot& snapshot, SpaceId id) {
    const auto found = std::ranges::find(snapshot.spaces, id, &SpaceVisual::id);
    return found == snapshot.spaces.end() ? nullptr : &*found;
}

QuadInstance target_instance(const SpaceVisual& space, InteractionTargetKind kind) {
    auto bounds = space.bounds;
    auto fill = Color{0.0F, 0.0F, 0.0F, 0.0F};
    auto border = Color{0.86F, 0.69F, 0.30F, 0.88F};
    auto border_width = 0.045F;
    auto shape = SpaceShape::rounded_rectangle;
    auto layer = 3.0F;
    switch (kind) {
    case InteractionTargetKind::quiet_move: {
        const auto center = bounds.center();
        const auto radius = std::min(bounds.width(), bounds.height()) * 0.105F;
        bounds = {{center.x - radius, center.y - radius},
                  {center.x + radius, center.y + radius}};
        fill = {0.83F, 0.72F, 0.46F, 0.78F};
        border_width = 0.0F;
        shape = SpaceShape::circle;
        break;
    }
    case InteractionTargetKind::capture:
        border = {0.92F, 0.34F, 0.28F, 0.94F};
        border_width = 0.075F;
        shape = SpaceShape::circle;
        break;
    case InteractionTargetKind::castle:
        border = {0.45F, 0.72F, 0.94F, 0.92F};
        border_width = 0.055F;
        break;
    case InteractionTargetKind::promotion:
        border = {0.73F, 0.48F, 0.96F, 0.96F};
        border_width = 0.065F;
        shape = SpaceShape::circle;
        break;
    case InteractionTargetKind::draw_claim:
        fill = {0.76F, 0.58F, 0.22F, 0.13F};
        border = {0.96F, 0.81F, 0.43F, 0.9F};
        break;
    case InteractionTargetKind::hover:
        fill = {0.76F, 0.84F, 0.95F, 0.08F};
        border = {0.72F, 0.82F, 0.95F, 0.55F};
        border_width = 0.025F;
        layer = 2.7F;
        break;
    case InteractionTargetKind::keyboard_focus:
        border = {0.56F, 0.86F, 1.0F, 0.96F};
        border_width = 0.045F;
        layer = 3.2F;
        break;
    case InteractionTargetKind::selection:
        fill = {0.91F, 0.67F, 0.18F, 0.19F};
        border = {0.96F, 0.76F, 0.31F, 0.96F};
        border_width = 0.055F;
        layer = 2.8F;
        break;
    case InteractionTargetKind::invalid:
        fill = {0.75F, 0.08F, 0.10F, 0.16F};
        border = {0.98F, 0.22F, 0.20F, 0.96F};
        border_width = 0.065F;
        break;
    }
    return solid_instance(bounds, fill, shape, space.rotation, border_width, border, layer);
}

Vec2 batched_center(const PieceVisual& piece,
                    std::span<const PieceAnimation* const> animations,
                    std::chrono::steady_clock::time_point now) {
    const auto found = std::ranges::lower_bound(
        animations, piece.id, {}, [](const PieceAnimation* animation) {
            return animation->entity;
        });
    if (found == animations.end() || (*found)->entity != piece.id ||
        (*found)->duration.count() <= 0) {
        return piece.center;
    }
    const auto* animation = *found;
    auto elapsed = std::chrono::duration<float>{now - animation->start}.count();
    const auto duration = std::chrono::duration<float>{animation->duration}.count();
    float progress = elapsed / duration;
    if (animation->loop) {
        elapsed = std::fmod(std::max(elapsed, 0.0F), duration);
        const auto phase = elapsed / duration;
        progress = 0.5F - 0.5F * std::cos(phase * 6.283185307179586F);
    }
    return lerp(animation->from, animation->to, progress);
}

} // namespace

void SpriteBatch::prepare(const RenderSnapshot& snapshot, const TextureAtlas* atlas,
                          const InteractionState& interaction,
                          std::chrono::steady_clock::time_point now) {
    instances_.clear();
    instances_.reserve(snapshot.spaces.size() + snapshot.links.size() + snapshot.shapes.size() +
                       interaction.targets.size() + snapshot.decorations.size() +
                       snapshot.pieces.size() + (interaction.selected ? 1U : 0U) +
                       (interaction.drag ? 1U : 0U));
    const auto revision = snapshot.static_revision != 0U ? snapshot.static_revision
                                                         : snapshot.revision;
    if (!static_ready_ || revision != static_revision_) {
        static_instances_.clear();
        static_instances_.reserve(snapshot.spaces.size() + snapshot.links.size());
        for (const auto& space : snapshot.spaces) {
            static_instances_.push_back(solid_instance(space.bounds, space.color, space.shape,
                                                       space.rotation, space.border_width,
                                                       space.border));
        }
        for (const auto& link : snapshot.links) {
            static_instances_.push_back(link_instance(link));
        }
        static_revision_ = revision;
        static_ready_ = true;
        ++static_build_count_;
    }
    instances_.insert(instances_.end(), static_instances_.begin(), static_instances_.end());
    for (const auto& shape : snapshot.shapes) {
        instances_.push_back(solid_instance(shape.bounds, shape.color, shape.shape,
                                            shape.rotation, shape.border_width,
                                            shape.border, shape.layer));
    }
    if (interaction.hovered) {
        if (const auto* space = visual_space(snapshot, *interaction.hovered)) {
            instances_.push_back(target_instance(*space, InteractionTargetKind::hover));
        }
    }
    if (interaction.keyboard_focus) {
        if (const auto* space = visual_space(snapshot, *interaction.keyboard_focus)) {
            instances_.push_back(
                target_instance(*space, InteractionTargetKind::keyboard_focus));
        }
    }
    if (interaction.selected) {
        const auto* piece = find_piece(snapshot, *interaction.selected);
        if (piece != nullptr) {
            if (const auto* space = visual_space(snapshot, piece->location)) {
                instances_.push_back(
                    target_instance(*space, InteractionTargetKind::selection));
            }
        }
    }
    for (const auto& target : interaction.targets) {
        if (const auto* space = visual_space(snapshot, target.space)) {
            instances_.push_back(target_instance(*space, target.kind));
        }
    }

    for (const auto& decoration : snapshot.decorations) {
        const auto* region = atlas != nullptr ? atlas->region(decoration.sprite) : nullptr;
        instances_.push_back(QuadInstance{
            {decoration.center.x, decoration.center.y},
            {decoration.size.x, decoration.size.y},
            {decoration.tint.red, decoration.tint.green, decoration.tint.blue,
             decoration.tint.alpha},
            region != nullptr
                ? std::array<float, 4U>{region->u_min, region->v_min, region->u_max,
                                        region->v_max}
                : std::array<float, 4U>{0.0F, 0.0F, 1.0F, 1.0F},
            region != nullptr ? 1.0F : 0.0F,
            decoration.rotation,
            static_cast<float>(SpaceShape::rectangle),
            0.0F,
            {},
            region != nullptr ? static_cast<float>(region->page) : 0.0F,
            region != nullptr && region->nearest ? 1.0F : 0.0F,
            decoration.layer,
        });
    }

    const auto ordered_revision = snapshot.dynamic_revision != 0U
                                      ? snapshot.dynamic_revision
                                      : snapshot.revision;
    if (ordered_snapshot_ != &snapshot || ordered_revision_ != ordered_revision) {
        ordered_pieces_.clear();
        ordered_pieces_.reserve(snapshot.pieces.size());
        for (const auto& piece : snapshot.pieces) {
            ordered_pieces_.push_back(&piece);
        }
        std::ranges::sort(ordered_pieces_, [](const PieceVisual* left,
                                              const PieceVisual* right) {
            if (left->layer != right->layer) {
                return left->layer < right->layer;
            }
            return left->id < right->id;
        });
        ordered_animations_.clear();
        ordered_animations_.reserve(snapshot.animations.size());
        for (const auto& animation : snapshot.animations) {
            ordered_animations_.push_back(&animation);
        }
        std::ranges::sort(ordered_animations_, {}, [](const PieceAnimation* animation) {
            return animation->entity;
        });
        ordered_snapshot_ = &snapshot;
        ordered_revision_ = ordered_revision;
    }
    for (const auto* piece : ordered_pieces_) {
        if (interaction.drag && interaction.drag->active &&
            interaction.drag->entity == piece->id) {
            continue;
        }
        const auto* region = atlas != nullptr ? atlas->region(piece->sprite) : nullptr;
        const auto center = batched_center(*piece, ordered_animations_, now);
        instances_.push_back(QuadInstance{
            {center.x, center.y},
            {piece->size.x, piece->size.y},
            {piece->tint.red, piece->tint.green, piece->tint.blue, piece->tint.alpha},
            region != nullptr
                ? std::array<float, 4U>{region->u_min, region->v_min, region->u_max,
                                        region->v_max}
                : std::array<float, 4U>{0.0F, 0.0F, 1.0F, 1.0F},
            region != nullptr ? 1.0F : 0.0F,
            piece->rotation,
            static_cast<float>(SpaceShape::rectangle),
            piece->outline_width,
            {piece->outline.red, piece->outline.green, piece->outline.blue,
             piece->outline.alpha},
            region != nullptr ? static_cast<float>(region->page) : 0.0F,
            region != nullptr && region->nearest ? 1.0F : 0.0F,
            piece->layer,
        });
    }
    if (interaction.drag && interaction.drag->active) {
        const auto* piece = find_piece(snapshot, interaction.drag->entity);
        const auto* region = piece != nullptr && atlas != nullptr
                                 ? atlas->region(piece->sprite) : nullptr;
        if (piece != nullptr) {
            auto tint = piece->tint;
            tint.alpha *= interaction.drag->valid ? 0.96F : 0.62F;
            instances_.push_back(QuadInstance{
                {interaction.drag->pointer.x, interaction.drag->pointer.y + 0.08F},
                {piece->size.x * 1.08F, piece->size.y * 1.08F},
                {tint.red, tint.green, tint.blue, tint.alpha},
                region != nullptr
                    ? std::array<float, 4U>{region->u_min, region->v_min, region->u_max,
                                            region->v_max}
                    : std::array<float, 4U>{0.0F, 0.0F, 1.0F, 1.0F},
                region != nullptr ? 1.0F : 0.0F,
                piece->rotation,
                static_cast<float>(SpaceShape::rectangle),
                0.045F,
                interaction.drag->valid
                    ? std::array<float, 4U>{0.96F, 0.78F, 0.34F, 0.88F}
                    : std::array<float, 4U>{0.96F, 0.18F, 0.16F, 0.9F},
                region != nullptr ? static_cast<float>(region->page) : 0.0F,
                region != nullptr && region->nearest ? 1.0F : 0.0F,
                50.0F,
            });
        }
    }
    for (const auto& bar : snapshot.bars) {
        const auto maximum = std::max(bar.maximum, 0.0001F);
        const auto progress = std::clamp(bar.value / maximum, 0.0F, 1.0F);
        instances_.push_back(solid_instance(bar.bounds, bar.background,
                                            SpaceShape::rounded_rectangle, 0.0F, 0.0F, {},
                                            bar.layer));
        auto fill = bar.bounds;
        fill.maximum.x = fill.minimum.x + fill.width() * progress;
        instances_.push_back(
            solid_instance(fill, bar.fill, SpaceShape::rounded_rectangle, 0.0F, 0.0F, {},
                           bar.layer + 0.001F));
    }
    const auto append_effects = [&](EffectBlend blend) {
        for (const auto& effect : snapshot.effects) {
            if (effect.blend != blend || effect.duration.count() <= 0 ||
                now >= effect.start + effect.duration) {
                continue;
            }
            const auto elapsed = std::chrono::duration<float>{now - effect.start}.count();
            const auto duration = std::chrono::duration<float>{effect.duration}.count();
            const auto progress = std::clamp(elapsed / duration, 0.0F, 1.0F);
            const bool travels = effect.kind == EffectKind::projectile ||
                                 effect.kind == EffectKind::poison;
            const auto center = travels ? lerp(effect.from, effect.to, progress) : effect.to;
            auto color = effect.color;
            color.alpha *= 1.0F - progress;
            auto scale = effect.initial_scale +
                         (effect.final_scale - effect.initial_scale) * progress;
            if (effect.kind == EffectKind::impact) {
                scale *= 0.5F + progress;
            } else if (effect.kind == EffectKind::pulse || effect.kind == EffectKind::check) {
                scale *= 0.88F + 0.16F * std::sin(progress * 12.566370614359172F);
            }
            const Rect bounds{
                {center.x - effect.size * scale, center.y - effect.size * scale},
                {center.x + effect.size * scale, center.y + effect.size * scale}};
            instances_.push_back(solid_instance(bounds, color, SpaceShape::circle, 0.0F,
                                                0.0F, {}, effect.layer));
        }
    };
    append_effects(EffectBlend::alpha);
    std::ranges::stable_sort(instances_, {}, &QuadInstance::layer);
    additive_offset_ = instances_.size();
    append_effects(EffectBlend::additive);
    std::ranges::stable_sort(
        std::ranges::subrange{instances_.begin() +
                                  static_cast<std::ptrdiff_t>(additive_offset_),
                              instances_.end()},
        {}, &QuadInstance::layer);
}

void SpriteBatch::prepare(const RenderSnapshot& snapshot, const TextureAtlas* atlas,
                          std::optional<SpaceId> focused,
                          std::optional<EntityId> selected,
                          std::span<const SpaceId> legal_destinations,
                          std::chrono::steady_clock::time_point now) {
    InteractionState interaction;
    interaction.selected = selected;
    interaction.keyboard_focus = focused;
    interaction.targets.reserve(legal_destinations.size());
    for (const auto destination : legal_destinations) {
        interaction.targets.push_back(
            InteractionTarget{destination, InteractionTargetKind::quiet_move});
    }
    prepare(snapshot, atlas, interaction, now);
}

} // namespace ludus
