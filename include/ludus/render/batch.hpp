#pragma once

#include "ludus/render/atlas.hpp"
#include "ludus/render/snapshot.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace ludus {

/// Backend-neutral quad data, ordered back-to-front and ready for one instanced draw.
struct QuadInstance {
    std::array<float, 2U> center;
    std::array<float, 2U> size;
    std::array<float, 4U> color;
    std::array<float, 4U> uv;
    float textured{0.0F};
    float rotation{0.0F};
    float shape{0.0F};
    float border_width{0.0F};
    std::array<float, 4U> border_color{};
    float atlas_page{0.0F};
    float nearest{0.0F};
    float layer{0.0F};
};

static_assert(std::is_standard_layout_v<QuadInstance>);

/// Reusable CPU-side batch storage. Capacity is retained between frames.
class SpriteBatch {
  public:
    void prepare(const RenderSnapshot& snapshot, const TextureAtlas* atlas,
                 const InteractionState& interaction,
                 std::chrono::steady_clock::time_point now);
    void prepare(const RenderSnapshot& snapshot, const TextureAtlas* atlas,
                 std::optional<SpaceId> focused, std::optional<EntityId> selected,
                 std::span<const SpaceId> legal_destinations,
                 std::chrono::steady_clock::time_point now);

    [[nodiscard]] std::span<const QuadInstance> instances() const noexcept {
        return instances_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return instances_.capacity(); }
    [[nodiscard]] std::size_t additive_offset() const noexcept { return additive_offset_; }
    [[nodiscard]] std::size_t static_build_count() const noexcept { return static_build_count_; }

  private:
    std::vector<QuadInstance> instances_;
    std::vector<QuadInstance> static_instances_;
    std::vector<const PieceVisual*> ordered_pieces_;
    std::vector<const PieceAnimation*> ordered_animations_;
    const RenderSnapshot* ordered_snapshot_{nullptr};
    std::uint64_t ordered_revision_{0U};
    std::uint64_t static_revision_{0U};
    bool static_ready_{false};
    std::size_t static_build_count_{0U};
    std::size_t additive_offset_{0U};
};

} // namespace ludus
