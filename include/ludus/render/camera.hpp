#pragma once

#include "ludus/render/math.hpp"

namespace ludus {

class Camera2D {
  public:
    static constexpr float minimum_zoom = 0.25F;
    static constexpr float maximum_zoom = 8.0F;

    void resize(int width, int height) noexcept;
    void fit(Rect bounds, float padding = 1.08F) noexcept;
    void zoom_at(Vec2 screen_point, float factor) noexcept;
    void pan_pixels(Vec2 delta) noexcept;
    void rotate_at(Vec2 screen_point, float radians) noexcept;
    void set_rotation(float radians) noexcept;

    [[nodiscard]] Vec2 screen_to_world(Vec2 screen_point) const noexcept;
    [[nodiscard]] Vec2 world_to_screen(Vec2 world_point) const noexcept;
    [[nodiscard]] Vec2 center() const noexcept { return center_; }
    [[nodiscard]] float zoom() const noexcept { return zoom_; }
    [[nodiscard]] float rotation() const noexcept { return rotation_; }
    [[nodiscard]] float aspect_ratio() const noexcept;
    [[nodiscard]] float visible_half_height() const noexcept;
    [[nodiscard]] float visible_half_width() const noexcept;

  private:
    void update_fitted_extent() noexcept;

    int width_{1};
    int height_{1};
    Vec2 center_{};
    float content_half_width_{1.0F};
    float content_half_height_{1.0F};
    float fit_padding_{1.0F};
    float fitted_half_height_{1.0F};
    float zoom_{1.0F};
    float rotation_{0.0F};
};

} // namespace ludus
