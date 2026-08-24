#pragma once

#include "ludus/render/snapshot.hpp"
#include "ludus/rules/event.hpp"

#include <chrono>
#include <vector>

namespace ludus {

class EventAnimationAdapter {
  public:
    explicit EventAnimationAdapter(std::chrono::milliseconds move_duration =
                                       std::chrono::milliseconds{180})
        : move_duration_(move_duration) {}

    [[nodiscard]] std::vector<PieceAnimation>
    adapt(const EventBatch& batch, const RenderSnapshot& before, const RenderSnapshot& after,
          std::chrono::steady_clock::time_point start) const;

  private:
    std::chrono::milliseconds move_duration_;
};

} // namespace ludus
