#pragma once

#include "ludus/render/snapshot.hpp"
#include "ludus/render/player_view.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace ludus {

/// Single-producer/single-consumer latest-value exchange. Publication never exposes a
/// mutable snapshot and rendering never holds a simulation lock.
class RenderSnapshotExchange {
  public:
    [[nodiscard]] std::shared_ptr<const RenderSnapshot> publish(RenderSnapshot snapshot) {
        auto immutable = std::make_shared<const RenderSnapshot>(std::move(snapshot));
        latest_.store(immutable, std::memory_order_release);
        return immutable;
    }

    void publish(std::shared_ptr<const RenderSnapshot> snapshot) noexcept {
        latest_.store(std::move(snapshot), std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<const RenderSnapshot> load() const noexcept {
        return latest_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<std::shared_ptr<const RenderSnapshot>> latest_;
};

/// Atomic latest-value publication for the complete package-neutral player view.
class PlayerViewExchange {
  public:
    [[nodiscard]] std::shared_ptr<const PlayerView> publish(PlayerView view) {
        auto immutable = std::make_shared<const PlayerView>(std::move(view));
        latest_.store(immutable, std::memory_order_release);
        return immutable;
    }

    void publish(std::shared_ptr<const PlayerView> view) noexcept {
        latest_.store(std::move(view), std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<const PlayerView> load() const noexcept {
        return latest_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<std::shared_ptr<const PlayerView>> latest_;
};

} // namespace ludus
