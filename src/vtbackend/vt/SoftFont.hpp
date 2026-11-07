// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// DECDLD's Pt: what the downloaded glyphs are for.
enum class SoftFontUsage : uint8_t
{
    Text,     ///< 0 or 1: text, possibly narrower than its cell.
    FullCell, ///< 2: the glyph covers the whole cell (e.g. line drawing, sixel-art).
};

/// DECDLD's Pcss: the size of the character set being loaded.
enum class SoftCharsetSize : uint8_t
{
    Size94,
    Size96,
};

/// DECDLD's Pe: which previously loaded glyphs are erased before this load.
enum class SoftFontErase : uint8_t
{
    AllChars = 0,      ///< Erase every glyph of the font being loaded.
    ReloadedChars = 1, ///< Erase only the positions this load redefines.
    AllRenditions = 2, ///< Erase every soft font.
};

/// Why a DECDLD was refused.
enum class SoftFontError : uint8_t
{
    FontNumber,
    EraseControl,
    CellMatrix,
    FontSet,
    Usage,
    CharsetSize,
    TooLarge,
    Designator,
};

/// The raw DECDLD parameters, in sequence order. An absent parameter is 0.
struct DecdldParameters
{
    int pfn = 0;
    int pcn = 0;
    int pe = 0;
    int pcmw = 0;
    int pss = 0;
    int pt = 0;
    int pcmh = 0;
    int pcss = 0;
};

/// Where a declared cell size came from.
enum class SoftFontSizeSource : uint8_t
{
    Pixels, ///< Pcmw 0 or 5..16 (with Pcmh): a size in pixels.
    Matrix, ///< Pcmw 2/3/4: one of the 5x10, 6x10 and 7x10 matrices.
};

/// A validated DECDLD header.
struct SoftFontDeclaration
{
    uint8_t fontNumber = 0;
    uint8_t startPosition = 0x21;
    SoftFontErase erase = SoftFontErase::AllChars;
    uint8_t declaredWidth = 0;  ///< 0 = not declared.
    uint8_t declaredHeight = 0; ///< 0 = not declared.
    SoftFontSizeSource sizeSource = SoftFontSizeSource::Pixels;
    uint16_t columnsPerPage = 80; ///< 80 or 132.
    uint8_t linesPerPage = 24;    ///< 24, 36 or 48.
    SoftFontUsage usage = SoftFontUsage::Text;
    SoftCharsetSize charsetSize = SoftCharsetSize::Size94;

    auto operator<=>(SoftFontDeclaration const&) const = default;
};

/// Validates the DECDLD parameters the way DEC does.
/// @param parameters The raw parameters, absent = 0.
/// @return The declaration, or the reason the sequence is to be ignored.
[[nodiscard]] std::expected<SoftFontDeclaration, SoftFontError> decodeDecdld(
    DecdldParameters const& parameters);

/// The cell a soft font is rendered into.
struct SoftCellGeometry
{
    uint8_t fullWidth = 0;
    uint8_t fullHeight = 0;
    uint8_t textWidth = 0; ///< 0 = unclipped.
};

/// Works out which terminal's cell a font was designed for.
/// @param declaration The validated header.
/// @param usedWidth The widest sixel line of any glyph, in columns.
/// @param usedHeight The tallest glyph, in pixel rows (a multiple of 6).
[[nodiscard]] SoftCellGeometry detectTargetCell(SoftFontDeclaration const& declaration,
                                                uint8_t usedWidth,
                                                uint8_t usedHeight);

/// One soft font glyph, as a bitmap of exactly the font's full cell size.
struct SoftGlyph
{
    uint64_t id = 0;               ///< Content hash of (width, height, coverage); the atlas key.
    uint8_t width = 0;             ///< Equals the font's SoftCellGeometry full width.
    uint8_t height = 0;            ///< Equals the font's SoftCellGeometry full height.
    std::vector<uint8_t> coverage; ///< Row-major, top row first, each 0 or 1.
};

/// One loaded soft font.
struct SoftFont
{
    SoftFontDeclaration declaration;
    std::string designator; ///< Dscs: 0-2 intermediates followed by one final.
    SoftCellGeometry geometry;
    std::array<std::shared_ptr<SoftGlyph const>, 96> glyphs; ///< Index = position - 0x20; null = undefined.
    std::shared_ptr<SoftGlyph const> errorGlyph;             ///< Reverse '?', at the geometry's size.
};

/// Makes a glyph and stamps its content-hash id. The one factory the decoder, the state codec and the
/// daemon mirror all use, so identity never needs persisting or sending.
[[nodiscard]] std::shared_ptr<SoftGlyph const> makeSoftGlyph(uint8_t width,
                                                             uint8_t height,
                                                             std::vector<uint8_t> coverage);

/// Assembles a font from already-built glyphs (codec and mirror path; no sixel decoding), building the
/// error glyph from the geometry.
[[nodiscard]] SoftFont makeSoftFont(SoftFontDeclaration const& declaration,
                                    std::string designator,
                                    SoftCellGeometry geometry,
                                    std::array<std::shared_ptr<SoftGlyph const>, 96> glyphs);

/// Builds a font from a DECDLD body (Dscs followed by sixel data).
/// @param declaration The validated header.
/// @param body Everything after the DECDLD final byte.
/// @param previous The font currently under this number (for Pe=1 merging), or null.
[[nodiscard]] std::expected<SoftFont, SoftFontError> buildSoftFont(SoftFontDeclaration const& declaration,
                                                                   std::string_view body,
                                                                   SoftFont const* previous);

} // namespace vtbackend
