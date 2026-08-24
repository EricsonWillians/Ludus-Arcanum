#include "ludus/render/camera.hpp"

#include <algorithm>
#include <cmath>

namespace ludus {
namespace {

constexpr float tau = 6.2831853071795864769F;

Vec2 rotate(Vec2 value, float radians) noexcept {
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return {value.x * cosine - value.y * sine, value.x * sine + value.y * cosine};
}

} // namespace

void Camera2D::resize(int width, int height) noexcept {
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    update_fitted_extent();
}

void Camera2D::fit(Rect bounds, float padding) noexcept {
    center_ = bounds.center();
    content_half_width_ = std::max(bounds.width() * 0.5F, 0.01F);
    content_half_height_ = std::max(bounds.height() * 0.5F, 0.01F);
    fit_padding_ = std::max(padding, 1.0F);
    update_fitted_extent();
    zoom_ = 1.0F;
}

void Camera2D::zoom_at(Vec2 screen_point, float factor) noexcept {
    if (!std::isfinite(factor) || factor <= 0.0F) {
        return;
    }
    const auto anchored_world = screen_to_world(screen_point);
    zoom_ = std::clamp(zoom_ * factor, minimum_zoom, maximum_zoom);
    const auto shifted_world = screen_to_world(screen_point);
    center_ = center_ + (anchored_world - shifted_world);
}

void Camera2D::pan_pixels(Vec2 delta) noexcept {
    const Vec2 camera_delta{
        -delta.x * (2.0F * visible_half_width() / static_cast<float>(width_)),
        delta.y * (2.0F * visible_half_height() / static_cast<float>(height_))};
    center_ = center_ + rotate(camera_delta, rotation_);
}

void Camera2D::rotate_at(Vec2 screen_point, float radians) noexcept {
    if (!std::isfinite(radians)) {
        return;
    }
    const auto anchored_world = screen_to_world(screen_point);
    set_rotation(rotation_ + radians);
    const auto shifted_world = screen_to_world(screen_point);
    center_ = center_ + (anchored_world - shifted_world);
}

void Camera2D::set_rotation(float radians) noexcept {
    if (!std::isfinite(radians)) {
        return;
    }
    rotation_ = std::remainder(radians, tau);
}

Vec2 Camera2D::screen_to_world(Vec2 screen_point) const noexcept {
    const auto normalized_x = 2.0F * screen_point.x / static_cast<float>(width_) - 1.0F;
    const auto normalized_y = 1.0F - 2.0F * screen_point.y / static_cast<float>(height_);
    const Vec2 camera_point{normalized_x * visible_half_width(),
                            normalized_y * visible_half_height()};
    return center_ + rotate(camera_point, rotation_);
}

Vec2 Camera2D::world_to_screen(Vec2 world_point) const noexcept {
    const auto camera_point = rotate(world_point - center_, -rotation_);
    const auto normalized_x = camera_point.x / visible_half_width();
    const auto normalized_y = camera_point.y / visible_half_height();
    return {(normalized_x + 1.0F) * 0.5F * static_cast<float>(width_),
            (1.0F - normalized_y) * 0.5F * static_cast<float>(height_)};
}

float Camera2D::aspect_ratio() const noexcept {
    return static_cast<float>(width_) / static_cast<float>(height_);
}

float Camera2D::visible_half_height() const noexcept { return fitted_half_height_ / zoom_; }

float Camera2D::visible_half_width() const noexcept {
    return visible_half_height() * aspect_ratio();
}

void Camera2D::update_fitted_extent() noexcept {
    const auto half_width_as_height = content_half_width_ / aspect_ratio();
    fitted_half_height_ =
        std::max(content_half_height_, half_width_as_height) * fit_padding_;
}

} // namespace ludus
