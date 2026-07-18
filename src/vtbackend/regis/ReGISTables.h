// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <array>
#include <cstdint>

namespace vtbackend::regis
{

/// The ten top-level ReGIS commands, each introduced by a single (case-insensitive) letter.
///
/// @c None is the "no command" sentinel used before any command letter has been seen and after a
/// @c ; command terminator; it must remain the first enumerator so a zero-initialised dispatch table
/// means "not a command".
enum class Command : uint8_t
{
    None = 0,   ///< No active command (sentinel).
    Position,   ///< @c P — move the graphics cursor / manage the position stack / select page.
    Vector,     ///< @c V — draw lines and dots.
    Curve,      ///< @c C — circles, arcs and interpolated curves.
    Write,      ///< @c W — set writing controls (colour, pattern, mode, line width, shading).
    Text,       ///< @c T — draw text and set text attributes.
    ScreenCmd,  ///< @c S — screen erase, addressing, colour map, page, scale, scroll.
    Fill,       ///< @c F — fill a polygon or curve region.
    Load,       ///< @c L — load / define custom character-set glyphs.
    Report,     ///< @c R — query state and send a reply to the host.
    Macrograph, ///< @c \@ — define / invoke / clear stored command-string macros.
};

/// The writing mode selected by @c W(V|R|C|E): how a plotted pixel combines with the canvas.
///
/// @c Replace is the power-up default. The order matches the DEC enumeration
/// (overlay=1, replace=2, complement=3, erase=4) offset by the @c Replace default being the norm.
enum class WritingMode : uint8_t
{
    Overlay = 0, ///< @c W(V) — paint the foreground colour, leaving other planes untouched.
    Replace,     ///< @c W(R) — overwrite with the foreground colour (default).
    Complement,  ///< @c W(C) — invert the existing pixel.
    Erase,       ///< @c W(E) — paint the background colour.
};

/// Maps a byte to the ReGIS @c Command it introduces, or @c Command::None if it introduces none.
///
/// Command letters are case-insensitive, so both cases resolve to the same command. This is the
/// data-driven replacement for a hand-written @c switch: adding a command is a table entry.
constexpr inline std::array<Command, 256> CommandByLetter = [] {
    std::array<Command, 256> table {};
    auto set = [&table](char lower, char upper, Command cmd) {
        table[static_cast<uint8_t>(lower)] = cmd;
        table[static_cast<uint8_t>(upper)] = cmd;
    };
    set('p', 'P', Command::Position);
    set('v', 'V', Command::Vector);
    set('c', 'C', Command::Curve);
    set('w', 'W', Command::Write);
    set('t', 'T', Command::Text);
    set('s', 'S', Command::ScreenCmd);
    set('f', 'F', Command::Fill);
    set('l', 'L', Command::Load);
    set('r', 'R', Command::Report);
    table[static_cast<uint8_t>('@')] = Command::Macrograph;
    return table;
}();

/// @return the @c Command a byte introduces (case-insensitive), or @c Command::None.
[[nodiscard]] constexpr Command commandOf(char ch) noexcept
{
    return CommandByLetter[static_cast<uint8_t>(ch)];
}

/// A signed unit step in canvas pixel space (y grows downward).
struct PvDelta
{
    int dx;
    int dy;
};

/// The eight ReGIS pixel-vector directions, indexed by the digit @c 0..7.
///
/// Each digit in a pixel-vector run steps one @c W(M) multiplier unit in a compass direction:
/// 0=right, 1=upper-right, 2=up, 3=upper-left, 4=left, 5=lower-left, 6=down, 7=lower-right.
constexpr inline std::array<PvDelta, 8> PixelVectorDelta = { {
    { .dx = +1, .dy = 0 },
    { .dx = +1, .dy = -1 },
    { .dx = 0, .dy = -1 },
    { .dx = -1, .dy = -1 },
    { .dx = -1, .dy = 0 },
    { .dx = -1, .dy = +1 },
    { .dx = 0, .dy = +1 },
    { .dx = +1, .dy = +1 },
} };

/// The ten standard line patterns selected by @c W(P0..P9), each an 8-bit repeating mask.
///
/// A set bit paints, a clear bit skips; the mask is consumed one bit per plotted pixel and wraps.
/// Values are xterm's (graphics_regis.c): 0=none, 1=solid, then progressively sparser dashes/dots.
constexpr inline std::array<uint8_t, 10> StandardPattern = {
    0x00, 0xff, 0xf0, 0xe4, 0xaa, 0xea, 0x88, 0x84, 0xc8, 0x86,
};

/// The power-up line pattern: solid.
constexpr inline uint8_t DefaultPattern = 0xff;

/// A ReGIS text display / unit cell size in pixels.
struct CellSize
{
    uint16_t width;
    uint16_t height;
};

/// The 17 standard text display-cell sizes selected by @c T(S0..S16), in pixels.
///
/// These are xterm's values (get_standard_character_size). Sizes 0 and 1 share a 9-pixel width but
/// differ in height; from size 2 up the cell is @c 9n wide by @c 15n high. The irregular low end is
/// why this is an explicit table rather than a formula.
constexpr inline std::array<CellSize, 17> StandardTextSize = { {
    { .width = 9, .height = 10 },    // 0
    { .width = 9, .height = 20 },    // 1
    { .width = 18, .height = 30 },   // 2
    { .width = 27, .height = 45 },   // 3
    { .width = 36, .height = 60 },   // 4
    { .width = 45, .height = 75 },   // 5
    { .width = 54, .height = 90 },   // 6
    { .width = 63, .height = 105 },  // 7
    { .width = 72, .height = 120 },  // 8
    { .width = 81, .height = 135 },  // 9
    { .width = 90, .height = 150 },  // 10
    { .width = 99, .height = 165 },  // 11
    { .width = 108, .height = 180 }, // 12
    { .width = 117, .height = 195 }, // 13
    { .width = 126, .height = 210 }, // 14
    { .width = 135, .height = 225 }, // 15
    { .width = 144, .height = 240 }, // 16
} };

/// A named ReGIS colour: the letter used in a colour spec and its RGB value.
struct NamedColor
{
    char letter;
    RGBColor rgb;
};

/// The eight ReGIS named colours usable in a colour spec, e.g. @c W(I(R)) for red.
///
/// D=dark(black), R=red, G=green, B=blue, C=cyan, Y=yellow, M=magenta, W=white.
constexpr inline std::array<NamedColor, 8> NamedColors = { {
    { .letter = 'D', .rgb = RGBColor { 0, 0, 0 } },
    { .letter = 'R', .rgb = RGBColor { 255, 0, 0 } },
    { .letter = 'G', .rgb = RGBColor { 0, 255, 0 } },
    { .letter = 'B', .rgb = RGBColor { 0, 0, 255 } },
    { .letter = 'C', .rgb = RGBColor { 0, 255, 255 } },
    { .letter = 'Y', .rgb = RGBColor { 255, 255, 0 } },
    { .letter = 'M', .rgb = RGBColor { 255, 0, 255 } },
    { .letter = 'W', .rgb = RGBColor { 255, 255, 255 } },
} };

/// Number of colour registers on a VT340 (the map the @c W(I<n>) index selects into).
constexpr inline unsigned ColorRegisterCount = 16;

/// The default ReGIS addressing space (the @c S(A...) window), matching the VT340: 0..799 x 0..479.
constexpr inline unsigned DefaultAddressWidth = 800;
constexpr inline unsigned DefaultAddressHeight = 480;

/// Supersampling factor for the ReGIS pixel canvas.
///
/// The ReGIS addressing space is a fixed 800x480 logical grid; a canvas at that pixel resolution
/// upscales into a blocky image on today's larger and HiDPI displays. Rendering the canvas at this
/// integer multiple of the logical resolution -- and letting the image pipeline downscale it to fit
/// -- yields crisp text and graphics. It is a quality/performance trade-off: canvas memory and
/// per-commit fill/upload cost scale with the square of the factor. The @ref ReGISContext carries the
/// factor (see @ref ReGISContext::supersample); the Screen injects this value while headless tests
/// keep the identity factor 1, so their pixel-exact assertions are unaffected.
constexpr inline unsigned RegisSupersample = 2;

/// Upper bound, in logical pixels, on any ReGIS text cell dimension.
///
/// Cell dimensions arrive straight from the wire (@c T(S[w,h]), @c T(U[...]), @c T(M[...]),
/// @c T(H<n>)) and the text rasterizer allocates a width*height coverage mask per glyph, so an
/// unbounded value would let a host request a multi-gigabyte allocation. This bound is already larger
/// than the 800x480 canvas, yet caps the per-glyph mask to a safe size.
constexpr inline unsigned MaxTextCellExtent = 512;

/// Upper bound on the @c T(H<n>) text height multiplier (matching xterm's clamp), applied before it
/// is multiplied by the base cell height so the product cannot overflow.
constexpr inline unsigned MaxTextHeightMultiplier = 256;

/// Upper bound on the @c W(L<n>) line width in logical pixels. @c stamp draws a width*width brush per
/// plotted pixel, so an unbounded width straight off the wire is a denial-of-service; a line this wide
/// is already far broader than any legible stroke.
constexpr inline int MaxLineWidth = 64;

/// Upper bound on the @c W(M<n>) pixel-vector multiplier. It scales every pixel-vector step and is
/// added to the persistent (signed) graphics cursor, so an unbounded value overflows it.
constexpr inline unsigned MaxPixelVectorMultiplier = 4096;

/// Maximum nesting depth of option sets @c ( ... ), guarding the parser-thread stack against a
/// pathological run of opening parentheses.
constexpr inline int MaxOptionSetDepth = 64;

/// Bound on the magnitude of a graphics-cursor coordinate, in canvas pixels. Far larger than any real
/// canvas, it only clamps coordinates driven off-canvas by hostile input -- preventing signed-integer
/// overflow of the persistent cursor while leaving all on-canvas drawing untouched.
constexpr inline int MaxCanvasCoord = 1 << 20;

} // namespace vtbackend::regis
