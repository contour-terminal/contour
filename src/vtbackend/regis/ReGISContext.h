// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/regis/ReGISColor.h>
#include <vtbackend/regis/ReGISRasterizer.h>
#include <vtbackend/regis/ReGISTables.h>

#include <crispy/point.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace vtbackend::regis
{

/// The ReGIS addressing window set by @c S(A[x0,y0][x1,y1]): the user-coordinate rectangle mapped
/// onto the pixel canvas. The VT340 default is (0,0)..(799,479).
struct AddressWindow
{
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = static_cast<double>(DefaultAddressWidth) - 1.0;
    double y1 = static_cast<double>(DefaultAddressHeight) - 1.0;
};

/// The persistent state of a ReGIS interpreter.
///
/// ReGIS state carries across successive @c DCS...p...ST strings: position, colours, write controls
/// and the addressing window all survive until a @c Pmode 1/3 reset. This struct therefore lives on
/// the terminal across hooks; a fresh @ref ReGISParser references it each hook. It carries no I/O and
/// is a plain value, so parser tests can construct and inspect it directly.
struct ReGISContext
{
    // --- Position (in canvas pixel space) ---------------------------------------------------------
    crispy::point position { .x = 0, .y = 0 }; ///< The graphics cursor, in pixels.

    // --- Addressing ------------------------------------------------------------------------------
    AddressWindow window {}; ///< User-coordinate window mapped onto the canvas.
    ImageSize canvasSize { Width(DefaultAddressWidth),
                           Height(DefaultAddressHeight) }; ///< Canvas pixel dimensions.

    /// Supersampling factor: canvas buffer pixels per logical addressing unit (see @ref
    /// RegisSupersample). Coordinates scale for free through @ref userToPixel (the canvas is this many
    /// times larger than the addressing window), but pixel-unit quantities that bypass that mapping --
    /// line width, pattern/pixel-vector multipliers, text cell size -- are multiplied by it explicitly.
    /// Defaults to 1 (identity) so headless tests keep pixel-exact behaviour; the Screen injects the
    /// production factor. Preserved across @ref reset (it is a display property, not ReGIS state).
    unsigned supersample = 1;

    // --- Write controls (the W command) ----------------------------------------------------------
    WritingMode writingMode = WritingMode::Overlay; ///< How plotted pixels combine (VT340 default: overlay).
    unsigned foregroundRegister = 7;    ///< Foreground colour register index (VT340 default: white-ish).
    unsigned backgroundRegister = 0;    ///< Background colour register index (VT340 default: black).
    uint8_t pattern = DefaultPattern;   ///< 8-bit line pattern.
    bool negativePattern = false;       ///< Whether @c W(N) inverts the pattern.
    unsigned patternMultiplier = 1;     ///< Pixels per pattern bit.
    unsigned pixelVectorMultiplier = 1; ///< Pixels per pixel-vector step (the @c W(M) multiplier).
    int lineWidth = 1;                  ///< Line width in pixels.

    // Shading (W(S...)): fill the swept area between a primitive and a reference line.
    bool shadingEnabled = false;  ///< Whether shading is active.
    bool shadingVertical = false; ///< Reference is a vertical line (x) rather than horizontal (y).
    int shadingReference = 0;     ///< The reference coordinate in pixels.

    // --- Text controls (the T command) -----------------------------------------------------------
    unsigned textCellWidth = 9;   ///< Text display-cell width in pixels (standard size 0).
    unsigned textCellHeight = 10; ///< Text display-cell height in pixels (standard size 0).
    int textDirection = 0;        ///< String writing direction in degrees (0 = rightward, CCW positive).
    int textSlant = 0;            ///< Character slant in degrees (best-effort).

    // --- Colour ----------------------------------------------------------------------------------
    ReGISColorRegisters registers {}; ///< The colour-register map.
    bool backgroundOpaque = false;    ///< Whether an erase paints an opaque background (vs. transparent).

    // --- Position / vector stack (P(B)/(S)...(E)) -------------------------------------------------
    std::vector<crispy::point> positionStack {}; ///< Saved positions for bounded/unbounded stacks.

    // --- Macrographs (@ command) -----------------------------------------------------------------
    std::unordered_map<char, std::string> macrographs {}; ///< Stored command-string macros by letter.

    /// Restores every field to its VT340 power-up default and clears the stacks.
    void reset();

    /// Maps a user coordinate to a canvas pixel through the current addressing window.
    /// @param userX,userY The user-space coordinate.
    /// @return The corresponding canvas pixel (clamped to int range).
    [[nodiscard]] crispy::point userToPixel(double userX, double userY) const noexcept;

    /// Maps a canvas pixel back to user coordinates (used by @c R(P) position reports).
    /// @param p The canvas pixel.
    /// @return The corresponding user-space coordinate (x, y).
    [[nodiscard]] std::pair<double, double> pixelToUser(crispy::point p) const noexcept;

    /// Scales a user-space delta to a pixel-space delta along each axis (for relative coordinates).
    /// @param dx,dy The user-space delta.
    /// @return The pixel-space delta.
    [[nodiscard]] crispy::point userDeltaToPixel(double dx, double dy) const noexcept;

    /// @return the drawing @ref Pen assembled from the current write controls and foreground colour.
    [[nodiscard]] Pen currentPen() const noexcept;

    /// @return the background colour used by an erase, honouring @ref backgroundOpaque.
    [[nodiscard]] RGBAColor backgroundColor() const noexcept;
};

} // namespace vtbackend::regis
