// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <compare>

namespace core::tui
{

/// A 2D point with x (column) and y (row) coordinates.
struct Point
{
    int x = 0; ///< Column (0-based)
    int y = 0; ///< Row (0-based)

    constexpr bool operator==(Point const&) const noexcept = default;
    constexpr auto operator<=>(Point const&) const noexcept = default;

    /// Offset this point by the given delta.
    [[nodiscard]] constexpr Point offset(int dx, int dy) const noexcept
    {
        return { .x = x + dx, .y = y + dy };
    }
};

/// A 2D size with width and height.
struct Size
{
    int width = 0;
    int height = 0;

    constexpr bool operator==(Size const&) const noexcept = default;

    /// Returns true if either dimension is zero or negative.
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

    /// Returns the total area (width * height).
    [[nodiscard]] constexpr int area() const noexcept { return width * height; }
};

/// A rectangle defined by position (x, y) and dimensions (width, height).
///
/// Coordinates are 0-based. The rectangle includes cells from (x, y) to
/// (x + width - 1, y + height - 1) inclusive.
struct Rect
{
    int x = 0;      ///< Left column (0-based)
    int y = 0;      ///< Top row (0-based)
    int width = 0;  ///< Width in columns
    int height = 0; ///< Height in rows

    // --- Factory Methods ---

    /// Create a rectangle from position and size.
    [[nodiscard]] static constexpr Rect fromXYWH(int x, int y, int w, int h) noexcept
    {
        return { .x = x, .y = y, .width = w, .height = h };
    }

    /// Create a rectangle from two corner points (inclusive).
    [[nodiscard]] static constexpr Rect fromCorners(Point topLeft, Point bottomRight) noexcept
    {
        return { .x = topLeft.x,
                 .y = topLeft.y,
                 .width = bottomRight.x - topLeft.x,
                 .height = bottomRight.y - topLeft.y };
    }

    /// Create a rectangle from position and size.
    [[nodiscard]] static constexpr Rect fromPositionSize(Point pos, Size size) noexcept
    {
        return { .x = pos.x, .y = pos.y, .width = size.width, .height = size.height };
    }

    // --- Accessors ---

    /// Left edge (x coordinate).
    [[nodiscard]] constexpr int left() const noexcept { return x; }

    /// Top edge (y coordinate).
    [[nodiscard]] constexpr int top() const noexcept { return y; }

    /// Right edge (x + width), exclusive.
    [[nodiscard]] constexpr int right() const noexcept { return x + width; }

    /// Bottom edge (y + height), exclusive.
    [[nodiscard]] constexpr int bottom() const noexcept { return y + height; }

    /// Top-left corner.
    [[nodiscard]] constexpr Point topLeft() const noexcept { return { .x = x, .y = y }; }

    /// Top-right corner.
    [[nodiscard]] constexpr Point topRight() const noexcept { return { .x = right(), .y = y }; }

    /// Bottom-left corner.
    [[nodiscard]] constexpr Point bottomLeft() const noexcept { return { .x = x, .y = bottom() }; }

    /// Bottom-right corner.
    [[nodiscard]] constexpr Point bottomRight() const noexcept { return { .x = right(), .y = bottom() }; }

    /// Size of the rectangle.
    [[nodiscard]] constexpr Size size() const noexcept { return { .width = width, .height = height }; }

    /// Position (top-left corner).
    [[nodiscard]] constexpr Point position() const noexcept { return { .x = x, .y = y }; }

    // --- Queries ---

    /// Returns true if the rectangle has zero or negative area.
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

    /// Returns the area of the rectangle.
    [[nodiscard]] constexpr int area() const noexcept { return width * height; }

    /// Returns true if the point (px, py) is inside this rectangle.
    [[nodiscard]] constexpr bool contains(int px, int py) const noexcept
    {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    /// Returns true if the point is inside this rectangle.
    [[nodiscard]] constexpr bool contains(Point p) const noexcept { return contains(p.x, p.y); }

    /// Returns true if this rectangle fully contains the other rectangle.
    [[nodiscard]] constexpr bool contains(Rect const& other) const noexcept
    {
        return other.x >= x && other.right() <= right() && other.y >= y && other.bottom() <= bottom();
    }

    /// Returns true if this rectangle intersects with the other rectangle.
    [[nodiscard]] constexpr bool intersects(Rect const& other) const noexcept
    {
        return x < other.right() && right() > other.x && y < other.bottom() && bottom() > other.y;
    }

    // --- Transformations ---

    /// Returns a new rectangle offset by (dx, dy).
    [[nodiscard]] constexpr Rect offset(int dx, int dy) const noexcept
    {
        return { .x = x + dx, .y = y + dy, .width = width, .height = height };
    }

    /// Returns a new rectangle offset by the given point.
    [[nodiscard]] constexpr Rect offset(Point delta) const noexcept { return offset(delta.x, delta.y); }

    /// Returns a new rectangle with position set to (newX, newY).
    [[nodiscard]] constexpr Rect withPosition(int newX, int newY) const noexcept
    {
        return { .x = newX, .y = newY, .width = width, .height = height };
    }

    /// Returns a new rectangle with position set to the given point.
    [[nodiscard]] constexpr Rect withPosition(Point pos) const noexcept
    {
        return { .x = pos.x, .y = pos.y, .width = width, .height = height };
    }

    /// Returns a new rectangle with size set to (newWidth, newHeight).
    [[nodiscard]] constexpr Rect withSize(int newWidth, int newHeight) const noexcept
    {
        return { .x = x, .y = y, .width = newWidth, .height = newHeight };
    }

    /// Returns a new rectangle with size set to the given size.
    [[nodiscard]] constexpr Rect withSize(Size newSize) const noexcept
    {
        return { .x = x, .y = y, .width = newSize.width, .height = newSize.height };
    }

    /// Returns a new rectangle shrunk by `amount` on all sides.
    [[nodiscard]] constexpr Rect inset(int amount) const noexcept
    {
        return {
            .x = x + amount, .y = y + amount, .width = width - (2 * amount), .height = height - (2 * amount)
        };
    }

    /// Returns a new rectangle shrunk by different amounts horizontally and vertically.
    [[nodiscard]] constexpr Rect inset(int horizontal, int vertical) const noexcept
    {
        return { .x = x + horizontal,
                 .y = y + vertical,
                 .width = width - (2 * horizontal),
                 .height = height - (2 * vertical) };
    }

    /// Returns a new rectangle shrunk by different amounts on each side.
    [[nodiscard]] constexpr Rect inset(int left, int top, int right, int bottom) const noexcept
    {
        return {
            .x = x + left, .y = y + top, .width = width - left - right, .height = height - top - bottom
        };
    }

    /// Returns a new rectangle expanded by `amount` on all sides.
    [[nodiscard]] constexpr Rect expand(int amount) const noexcept { return inset(-amount); }

    /// Returns the intersection of this rectangle with another.
    /// If the rectangles don't intersect, returns an empty rectangle.
    [[nodiscard]] constexpr Rect intersect(Rect const& other) const noexcept
    {
        int const newX = std::max(x, other.x);
        int const newY = std::max(y, other.y);
        int const newRight = std::min(right(), other.right());
        int const newBottom = std::min(bottom(), other.bottom());

        if (newRight <= newX || newBottom <= newY)
            return {}; // No intersection

        return { .x = newX, .y = newY, .width = newRight - newX, .height = newBottom - newY };
    }

    /// Returns the smallest rectangle that contains both this and the other rectangle.
    [[nodiscard]] constexpr Rect unite(Rect const& other) const noexcept
    {
        if (empty())
            return other;
        if (other.empty())
            return *this;

        int const newX = std::min(x, other.x);
        int const newY = std::min(y, other.y);
        int const newRight = std::max(right(), other.right());
        int const newBottom = std::max(bottom(), other.bottom());

        return { .x = newX, .y = newY, .width = newRight - newX, .height = newBottom - newY };
    }

    // --- Comparison ---

    constexpr bool operator==(Rect const&) const noexcept = default;
};

} // namespace core::tui
