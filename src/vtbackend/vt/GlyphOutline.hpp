// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// Why a Glyph Protocol registration is refused; each is one `reason=` of the `r` reply.
enum class GlyphRegisterError : uint8_t
{
    OutOfNamespace,       ///< The codepoint is outside the registration block.
    CompositeUnsupported, ///< The record is a composite glyph.
    HintingUnsupported,   ///< The record carries hinting instructions.
    MalformedPayload,     ///< The payload is not a simple glyph, or a sizing parameter is invalid.
    PayloadTooLarge,      ///< The payload is over MaxGlyphPayloadBytes once decoded from base64.
    OutlineTooLarge,      ///< The outline has more than MaxGlyphOutlinePoints points.
};

/// @return The `reason=` value that names @p error on the wire.
[[nodiscard]] std::string_view toString(GlyphRegisterError error) noexcept;

/// The largest payload a registration may carry, in bytes after base64 decoding.
inline constexpr size_t MaxGlyphPayloadBytes = size_t { 64 } * 1024;

/// The most points an outline may decode to: 64 KiB at the protocol's 12 bytes a point.
inline constexpr size_t MaxGlyphOutlinePoints = MaxGlyphPayloadBytes / 12;

/// Whether an outline point lies on the curve or is the control point of a quadratic Bezier.
enum class GlyphPointKind : uint8_t
{
    OffCurve,
    OnCurve,
};

/// One point of an outline, in the units the glyph was authored in, Y up.
struct GlyphOutlinePoint
{
    int32_t x = 0;
    int32_t y = 0;
    GlyphPointKind kind = GlyphPointKind::OffCurve;

    auto operator<=>(GlyphOutlinePoint const&) const = default;
};

/// The box around every point of an outline, control points included.
struct GlyphOutlineBounds
{
    int32_t xMin = 0;
    int32_t yMin = 0;
    int32_t xMax = 0;
    int32_t yMax = 0;

    auto operator<=>(GlyphOutlineBounds const&) const = default;
};

/// A decoded OpenType `glyf` simple glyph: closed contours of on- and off-curve points.
struct GlyphOutline
{
    std::vector<GlyphOutlinePoint> points; ///< Every contour's points, one contour after another.
    std::vector<uint16_t> contourEnds;     ///< Per contour, the index in points of its last point.
    /// Computed from the points: the bounds the record states are the sender's claim, and are not read.
    GlyphOutlineBounds bounds {};

    /// @param index Which contour, below contourEnds.size().
    /// @return The points of that contour.
    [[nodiscard]] std::span<GlyphOutlinePoint const> contour(size_t index) const noexcept
    {
        auto const first = index == 0 ? size_t { 0 } : size_t { contourEnds[index - 1] } + 1;
        return std::span { points }.subspan(first, size_t { contourEnds[index] } + 1 - first);
    }

    bool operator==(GlyphOutline const&) const = default;
};

/// Decodes an OpenType `glyf` simple-glyph record, the subset the Glyph Protocol accepts: no composite
/// glyph, no hinting instructions, and at most MaxGlyphOutlinePoints points. The point count is read from
/// the record's contour table and judged before any point is stored.
/// @param record The record, as it is stored in a font's `glyf` table.
/// @return The outline, or why the record is refused.
[[nodiscard]] std::expected<GlyphOutline, GlyphRegisterError> decodeGlyf(std::span<uint8_t const> record);

} // namespace vtbackend
