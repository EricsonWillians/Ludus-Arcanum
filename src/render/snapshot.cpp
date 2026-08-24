#include "ludus/render/snapshot.hpp"

#include <algorithm>
#include <cmath>

namespace ludus {

std::optional<SpaceId> pick_space(const RenderSnapshot& snapshot, Vec2 world_point) noexcept {
    for (auto space = snapshot.spaces.rbegin(); space != snapshot.spaces.rend(); ++space) {
        const auto center = space->bounds.center();
        auto local = world_point - center;
        const auto cosine = std::cos(-space->rotation);
        const auto sine = std::sin(-space->rotation);
        local = {local.x * cosine - local.y * sine, local.x * sine + local.y * cosine};
        const auto half_width = space->bounds.width() * 0.5F;
        const auto half_height = space->bounds.height() * 0.5F;
        bool inside = std::abs(local.x) <= half_width && std::abs(local.y) <= half_height;
        if (inside && space->shape == SpaceShape::circle) {
            const auto x = local.x / std::max(half_width, 0.0001F);
            const auto y = local.y / std::max(half_height, 0.0001F);
            inside = x * x + y * y <= 1.0F;
        } else if (inside && space->shape == SpaceShape::hexagon) {
            const auto x = std::abs(local.x) / std::max(half_width, 0.0001F);
            const auto y = std::abs(local.y) / std::max(half_height, 0.0001F);
            inside = x <= 1.0F && y <= 1.0F && x * 0.5F + y <= 1.0F;
        }
        if (inside) {
            return space->id;
        }
    }
    return std::nullopt;
}

const PieceVisual* find_piece(const RenderSnapshot& snapshot, EntityId entity) noexcept {
    const auto found = std::ranges::find(snapshot.pieces, entity, &PieceVisual::id);
    return found == snapshot.pieces.end() ? nullptr : &*found;
}

const PieceVisual* find_piece_at(const RenderSnapshot& snapshot, SpaceId space) noexcept {
    const auto found = std::ranges::find(snapshot.pieces, space, &PieceVisual::location);
    return found == snapshot.pieces.end() ? nullptr : &*found;
}

Vec2 animated_center(const RenderSnapshot& snapshot, const PieceVisual& piece,
                     std::chrono::steady_clock::time_point now) noexcept {
    const auto found = std::ranges::find(snapshot.animations, piece.id, &PieceAnimation::entity);
    if (found == snapshot.animations.end() || found->duration.count() <= 0) {
        return piece.center;
    }
    auto elapsed = std::chrono::duration<float>{now - found->start}.count();
    const auto duration = std::chrono::duration<float>{found->duration}.count();
    if (found->loop) {
        elapsed = std::fmod(std::max(elapsed, 0.0F), duration);
        const auto phase = elapsed / duration;
        return lerp(found->from, found->to,
                    0.5F - 0.5F * std::cos(phase * 6.283185307179586F));
    }
    return lerp(found->from, found->to, elapsed / duration);
}

bool has_active_animations(const RenderSnapshot& snapshot,
                           std::chrono::steady_clock::time_point now) noexcept {
    return std::ranges::any_of(snapshot.animations, [now](const PieceAnimation& animation) {
               return animation.duration.count() > 0 &&
                      (animation.loop || now < animation.start + animation.duration);
           }) ||
           std::ranges::any_of(snapshot.effects, [now](const EffectVisual& effect) {
               return effect.duration.count() > 0 && now < effect.start + effect.duration;
           });
}

} // namespace ludus
