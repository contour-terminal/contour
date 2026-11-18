// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/GlyphOutline.hpp>

#include <core/Base64.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Builds OpenType `glyf` simple-glyph records, the Glyph Protocol's payload, without a font.
namespace vtbackend::glyf_test
{

/// One contour, as the points a test wants decoded.
using Contour = std::vector<GlyphOutlinePoint>;

/// @param x The point's x.
/// @param y The point's y.
/// @return An on-curve point.
[[nodiscard]] constexpr GlyphOutlinePoint on(int32_t x, int32_t y) noexcept
{
    return { .x = x, .y = y, .kind = GlyphPointKind::OnCurve };
}

/// @param x The point's x.
/// @param y The point's y.
/// @return An off-curve (control) point.
[[nodiscard]] constexpr GlyphOutlinePoint off(int32_t x, int32_t y) noexcept
{
    return { .x = x, .y = y, .kind = GlyphPointKind::OffCurve };
}

inline void putU16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

/// Encodes @p contours as a simple-glyph record, every delta a 16-bit word and no flag repeated: the
/// plainest encoding there is, which a test of the compressed forms then varies by hand.
/// @param contours The contours.
/// @return The record.
[[nodiscard]] inline std::vector<uint8_t> record(std::span<Contour const> contours)
{
    auto out = std::vector<uint8_t> {};
    putU16(out, static_cast<uint16_t>(contours.size()));
    // The stated bounds: deliberately wrong, as the decoder must not read them.
    for ([[maybe_unused]] auto const _: { 0, 1, 2, 3 })
        putU16(out, 0x7FFF);

    auto end = uint16_t { 0 };
    for (auto const& contour: contours)
    {
        end = static_cast<uint16_t>(end + contour.size());
        putU16(out, static_cast<uint16_t>(end - 1));
    }
    putU16(out, 0); // instructionLength

    for (auto const& contour: contours)
        for (auto const& point: contour)
            out.push_back(point.kind == GlyphPointKind::OnCurve ? uint8_t { 0x01 } : uint8_t { 0x00 });

    for (auto const coordinate: { &GlyphOutlinePoint::x, &GlyphOutlinePoint::y })
    {
        auto previous = int32_t { 0 };
        for (auto const& contour: contours)
            for (auto const& point: contour)
            {
                putU16(out, static_cast<uint16_t>(static_cast<int16_t>(point.*coordinate - previous)));
                previous = point.*coordinate;
            }
    }
    return out;
}

/// @return A record of one contour: the square from (0, 0) to (1000, 1000).
[[nodiscard]] inline std::vector<uint8_t> squareRecord()
{
    auto const contours = std::vector<Contour> { { on(0, 0), on(0, 1000), on(1000, 1000), on(1000, 0) } };
    return record(contours);
}

/// @param bytes Any bytes.
/// @return @p bytes in base64, as a registration carries its payload.
[[nodiscard]] inline std::string base64(std::span<uint8_t const> bytes)
{
    auto text = std::string(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(), [](uint8_t byte) { return static_cast<char>(byte); });
    return core::base64::encode(text);
}

} // namespace vtbackend::glyf_test
