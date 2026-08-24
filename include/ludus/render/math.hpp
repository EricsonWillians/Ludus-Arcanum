#pragma once

#include <algorithm>
#include <compare>

namespace ludus {

struct Vec2 {
    float x{0.0F};
    float y{0.0F};

    auto operator<=>(const Vec2&) const = default;
};

[[nodiscard]] constexpr Vec2 operator+(Vec2 left, Vec2 right) noexcept {
    return {left.x + right.x, left.y + right.y};
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 left, Vec2 right) noexcept {
    return {left.x - right.x, left.y - right.y};
}

[[nodiscard]] constexpr Vec2 operator*(Vec2 value, float scale) noexcept {
    return {value.x * scale, value.y * scale};
}

[[nodiscard]] constexpr Vec2 lerp(Vec2 from, Vec2 to, float progress) noexcept {
    const auto clamped = std::clamp(progress, 0.0F, 1.0F);
    return from + (to - from) * clamped;
}

struct Rect {
    Vec2 minimum;
    Vec2 maximum;

    [[nodiscard]] constexpr float width() const noexcept { return maximum.x - minimum.x; }
    [[nodiscard]] constexpr float height() const noexcept { return maximum.y - minimum.y; }
    [[nodiscard]] constexpr Vec2 center() const noexcept {
        return {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F};
    }
    [[nodiscard]] constexpr bool contains(Vec2 point) const noexcept {
        return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y &&
               point.y <= maximum.y;
    }

    auto operator<=>(const Rect&) const = default;
};

struct Color {
    float red{1.0F};
    float green{1.0F};
    float blue{1.0F};
    float alpha{1.0F};

    auto operator<=>(const Color&) const = default;
};

} // namespace ludus
