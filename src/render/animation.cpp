#include "ludus/render/animation.hpp"

#include <variant>

namespace ludus {

std::vector<PieceAnimation>
EventAnimationAdapter::adapt(const EventBatch& batch, const RenderSnapshot& before,
                             const RenderSnapshot& after,
                             std::chrono::steady_clock::time_point start) const {
    std::vector<PieceAnimation> result;
    for (const auto& event : batch.events) {
        const auto* moved = std::get_if<EntityMoved>(&event.payload);
        if (moved == nullptr) {
            continue;
        }
        const auto* previous = find_piece(before, moved->entity);
        const auto* current = find_piece(after, moved->entity);
        if (previous == nullptr || current == nullptr || previous->center == current->center) {
            continue;
        }
        result.push_back(PieceAnimation{moved->entity, previous->center, current->center, start,
                                        move_duration_, false});
    }
    return result;
}

} // namespace ludus
