// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// DECDLD soft fonts as they travel on the native protocol (proto::SoftFontState).
///
/// Both directions live here so the daemon's capture and the mirror's rebuild agree bit for bit on the
/// bitmap layout. The mirror rebuilds fonts through vtbackend's own `makeSoftGlyph`/`makeSoftFont`, which
/// stamp a glyph's identity from its content: nothing about identity crosses the wire.

#include <vtbackend/vt/SoftFont.hpp>
#include <vtbackend/vt/SoftFontStore.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Why a set of wire fonts is not one a mirror takes.
enum class SoftFontWireError : uint8_t
{
    FontNumber,  ///< A font number above the last DECDLD has (2), or a number named twice.
    LoadRank,    ///< A load rank above the last font's.
    Designator,  ///< A designator longer than two intermediates and a final.
    Enumeration, ///< A usage or charset size no enumerator names.
    Glyph, ///< A cell outside 1..16 x 1..32, or a glyph out of its set, out of order, or not the cell's size.
};

/// Captures a store's fonts for the wire.
/// @param store The session's soft fonts (the caller holds the terminal lock).
/// @return One entry per loaded font, by font number.
[[nodiscard]] std::vector<proto::WireSoftFont> softFontsToWire(vtbackend::SoftFontStore const& store);

/// Rebuilds the fonts a peer sent.
///
/// Judges what the decoder cannot: a font number and rank outside DECDLD's three, a number sent twice, and
/// glyphs that are not the font's own cell size. Nothing is built into the caller's state until the whole set
/// is accepted, so a refused set leaves the mirror's fonts as they were. The bitmaps must be the size the
/// decoder checked (proto::softGlyphRowBytes by height).
/// @param fonts The fonts a SoftFontState carried.
/// @return The staged fonts, for SoftFontStore::replaceAll, or why the set was refused.
[[nodiscard]] std::expected<vtbackend::SoftFontSet, SoftFontWireError> softFontsFromWire(
    std::span<proto::WireSoftFont const> fonts);

} // namespace vthost
