// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>

#include <crispy/flags.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace vtbackend
{

/// Checksum extension flags, as selected by XTCHECKSUM (`CSI Ps # y`).
///
/// The unextended (zero) state is DEC-compatible; every bit *disables* one aspect of that default
/// behaviour. This mirrors xterm's `csDEC` / `cs*` flags one-for-one, which is what makes the
/// checksums Contour reports byte-identical to xterm's.
///
/// @note Bit 3 is worth reading twice: xterm's *documentation* (ctlseqs) describes it as "omit
///       checksum for cells not explicitly initialized", but both its implementation and its
///       observed behaviour do the opposite -- the bit makes uninitialized cells *count* (as a
///       blank) instead of being skipped. The behaviour, not the prose, is what is reproduced here.
enum class ChecksumFlag : uint8_t
{
    /// csPOSITIVE -- report the plain sum rather than its two's complement.
    Positive = 1 << 0,

    /// csATTRIBS -- do not fold the VT100 video attributes into each cell's value.
    NoAttributes = 1 << 1,

    /// csNOTRIM -- count blank cells rather than omitting them.
    KeepBlanks = 1 << 2,

    /// csDRAWN -- count cells that were never written to (as blanks) rather than skipping them.
    IncludeUndrawn = 1 << 3,

    /// csBYTE -- use the cell's codepoint as-is instead of mapping it into the DEC charset.
    RawCodepoint = 1 << 4,
};

using ChecksumFlags = crispy::flags<ChecksumFlag>;

/// One DEC video attribute, and the Contour cell flags that stand for it.
///
/// A row maps a *set* of flags to a single weight because a DEC terminal had fewer attributes than
/// Contour does: it knew one blink, where Contour distinguishes slow from rapid. Both must therefore
/// contribute 0x40, and a cell carrying both must still contribute it only once.
struct ChecksumAttributeWeight
{
    CellFlags flags;
    uint16_t weight;
};

/// The video attributes a DEC terminal folds into a cell's checksum value, and by how much.
///
/// This table is the single definition of "video attribute" for checksum purposes: it drives both
/// the value a cell contributes and whether an otherwise-blank cell is worth counting. Only the
/// attributes DEC terminals actually had appear here -- Contour's richer flags (italic, crossed out,
/// curly underline, ...) have no DEC counterpart and are therefore weightless, as they are in xterm.
inline constexpr auto ChecksumAttributeWeights = std::array {
    ChecksumAttributeWeight { .flags = CellFlags { CellFlag::CharacterProtected }, .weight = 0x04 },
    ChecksumAttributeWeight { .flags = CellFlags { CellFlag::Hidden }, .weight = 0x08 },
    ChecksumAttributeWeight { .flags = CellFlags { CellFlag::Underline }, .weight = 0x10 },
    ChecksumAttributeWeight { .flags = CellFlags { CellFlag::Inverse }, .weight = 0x20 },
    ChecksumAttributeWeight { .flags = CellFlags { CellFlag::Blinking } | CellFlag::RapidBlinking,
                              .weight = 0x40 },
    ChecksumAttributeWeight { .flags = CellFlags { CellFlag::Bold }, .weight = 0x80 },
};

/// One cell, reduced to just what the checksum depends on.
///
/// Keeping this free of `Cell` / `CellProxy` is what lets the algorithm be tested without standing
/// up a Screen: a test constructs cells from string literals.
struct ChecksumCell
{
    /// The cell's codepoints. **Empty means the cell was never written to** -- which is a different
    /// thing from a cell holding a space, and the two check-sum differently.
    std::span<char32_t const> codepoints;

    CellFlags flags {};
};

/// The value an undrawn cell contributes when it is counted at all, and the value a cell must have
/// to be considered blank.
inline constexpr uint16_t ChecksumBlank = 0x20;

/// The value xterm reports for a cell it cannot express in the target charset.
inline constexpr uint16_t ChecksumUnrepresentable = 0x1B; // ANSI ESC

/// Maps a codepoint the way xterm's `xtermCharSetDec()` does for the default (94-code, 7-bit)
/// DEC charset: anything outside Latin-1 is unrepresentable, and the rest is masked into 7 bits.
///
/// @note This is lossy on purpose -- it is what a DEC terminal reports. `U+00E9` checksums as
///       `0x69`, exactly as xterm does. Applications wanting the codepoint verbatim select
///       ChecksumFlag::RawCodepoint via XTCHECKSUM.
[[nodiscard]] constexpr uint16_t decCharsetChecksumValue(char32_t codepoint) noexcept
{
    if (codepoint < 0x20 || codepoint > 0xFF)
        return ChecksumUnrepresentable;
    return static_cast<uint16_t>(codepoint & 0x7F);
}

/// Accumulates the checksum of a rectangular area for DECRQCRA, one cell at a time.
///
/// Cells are fed in reading order; endOfLine() is called after each row of the rectangle. The
/// caller therefore owns the traversal (and with it the grid), while every decision lives here.
class RectangularAreaChecksum
{
  public:
    explicit constexpr RectangularAreaChecksum(ChecksumFlags flags) noexcept: _flags { flags } {}

    constexpr void addCell(ChecksumCell const& cell) noexcept
    {
        auto const written = !cell.codepoints.empty();

        // A cell that was never written to is not part of the picture at all, unless asked for.
        if (!written && !_flags.test(ChecksumFlag::KeepBlanks) && !_flags.test(ChecksumFlag::IncludeUndrawn))
            return;

        auto value = written ? codepointValue(cell.codepoints.front()) : ChecksumBlank;

        if (!_flags.test(ChecksumFlag::NoAttributes))
            value = static_cast<uint16_t>(value + attributeWeight(cell.flags));

        // A blank is omitted -- unless it was actually drawn, carries a video attribute, or is the
        // very first cell of the rectangle. That last exemption looks arbitrary because it is: it
        // falls out of xterm's implementation, and a rectangle of three undrawn cells therefore
        // checksums the same as one. Reproduced deliberately; see the tests.
        if (_first || value != ChecksumBlank || written || carriesVideoAttribute(cell.flags))
            _kept += value;

        _sum += value;

        // Combining marks are added raw, and only to the untrimmed sum -- so with the default
        // (trimming) flags they do not reach the reported checksum at all. xterm carries a
        // `FIXME - not counted if trimming blanks` here; matching it keeps us byte-compatible.
        if (written && !_flags.test(ChecksumFlag::RawCodepoint))
            for (auto const codepoint: cell.codepoints.subspan(1))
                _sum += static_cast<uint32_t>(codepoint);

        _first = _flags.test(ChecksumFlag::KeepBlanks);
    }

    constexpr void endOfLine() noexcept
    {
        if (!_flags.test(ChecksumFlag::KeepBlanks))
            _first = false;
    }

    [[nodiscard]] constexpr uint16_t result() const noexcept
    {
        auto value = _flags.test(ChecksumFlag::KeepBlanks) ? _sum : _kept;
        if (!_flags.test(ChecksumFlag::Positive))
            value = 0u - value;
        return static_cast<uint16_t>(value & 0xFFFFu);
    }

  private:
    [[nodiscard]] constexpr uint16_t codepointValue(char32_t codepoint) const noexcept
    {
        if (_flags.test(ChecksumFlag::RawCodepoint))
            return static_cast<uint16_t>(codepoint);
        return decCharsetChecksumValue(codepoint);
    }

    [[nodiscard]] static constexpr uint16_t attributeWeight(CellFlags cellFlags) noexcept
    {
        auto weight = uint16_t { 0 };
        for (auto const& [flags, value]: ChecksumAttributeWeights)
            if (cellFlags & flags)
                weight = static_cast<uint16_t>(weight + value);
        return weight;
    }

    [[nodiscard]] static constexpr bool carriesVideoAttribute(CellFlags cellFlags) noexcept
    {
        return std::ranges::any_of(ChecksumAttributeWeights,
                                   [cellFlags](auto const& entry) { return bool(cellFlags & entry.flags); });
    }

    ChecksumFlags _flags;
    uint32_t _sum = 0;  ///< every cell counted (xterm's `total`)
    uint32_t _kept = 0; ///< blanks omitted (xterm's `trimmed`)
    bool _first = true; ///< no cell of the rectangle has been counted yet
};

} // namespace vtbackend
