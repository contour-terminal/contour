// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file FrameDiff.hpp
/// @brief Two frames in, the bytes that turn the first into the second out.

#include <core/tui/Buffer.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/TuiCoreExport.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace core::tui
{

class TerminalOutput;

/// @brief How a frame diff reaches the cells it writes.
enum class DiffAddressing : std::uint8_t
{
    /// Each written cell is reached by an absolute cursor move (CUP); cells it skips cost nothing.
    /// What a fullscreen or fixed viewport uses, and the default.
    Absolute,
    /// Row by row from the cursor's current place, which must be column 0 of the first row: a
    /// carriage return, the row's cells (a skipped cell is a cursor move right), an erase to the
    /// end of the line, and a line feed to the next row. What the inline viewport uses, which does
    /// not know the row it is on. Always covers whole rows.
    Relative,
};

/// @brief Which cells a frame diff writes.
enum class DiffRepaint : std::uint8_t
{
    Changed, ///< Only cells that differ from the previous frame (the diff proper).
    All,     ///< Every cell, as if the previous frame were unknown (a full redraw).
};

/// @brief Whether a frame diff writes the OSC 8 framing of the next frame's hyperlink regions.
enum class DiffHyperlinks : std::uint8_t
{
    Omit, ///< Cells only; a region whose URI changed while its cells did not is not repainted.
    Emit, ///< Frame each written linked cell, and repaint a cell whose link changed target.
};

/// @brief Whether a frame diff writes the next frame's Sixel images.
enum class DiffImages : std::uint8_t
{
    Omit, ///< No image is written, and image regions are treated as ordinary cells.
    Emit, ///< A new or changed image is written; the cells it covers are left to it.
};

/// @brief Whether a frame diff writes the next frame's row marks.
enum class DiffRowMarks : std::uint8_t
{
    Omit, ///< No mark is written.
    Emit, ///< The mark governing each written cell is written ahead of it; a row whose marks changed repaints
          ///< whole.
};

/// @brief Whether a relative frame diff turns text reflow (DEC 2028) off on each row it writes.
enum class DiffReflow : std::uint8_t
{
    Keep,    ///< Leave reflow as the terminal has it.
    Inhibit, ///< Write DECRST 2028 at the start of each row. Ignored by absolute addressing.
};

/// @brief What a frame diff covers and what it writes besides cells.
///
/// The defaults are the plainest diff: absolute addressing over the whole frame, changed cells only,
/// and nothing out of band. A zero-initialized value means the same.
struct DiffOptions
{
    DiffAddressing addressing = DiffAddressing::Absolute; ///< How cells are reached.
    DiffRepaint repaint = DiffRepaint::Changed;           ///< Which cells are written.
    /// The cells covered, in buffer coordinates, clipped to the next frame; nothing means all of it.
    /// Relative addressing covers whole rows and reads only the rows from this.
    std::optional<Rect> area = std::nullopt;
    DiffHyperlinks hyperlinks = DiffHyperlinks::Omit; ///< OSC 8 framing.
    DiffImages images = DiffImages::Omit;             ///< Sixel images.
    DiffRowMarks rowMarks = DiffRowMarks::Omit;       ///< Zero-width row marks.
    DiffReflow reflow = DiffReflow::Keep;             ///< DEC 2028, relative addressing only.
};

/// @brief Writes the bytes that turn @p previous into @p next into @p out, without flushing it.
///
/// The one diff every flush path of @c Screen calls. It neither hides nor places the cursor -- that
/// is the frame driver's, which knows where the cursor should end up -- and writes nothing when the
/// two frames agree under absolute addressing. Frames of different sizes compare as far as both
/// reach; a cell outside @p previous counts as changed.
///
/// @param out Where the bytes are composed; written through its virtual members, so a
///            @c MockTerminalOutput sees each move and write.
/// @param previous The frame on the screen. Read only with @c DiffRepaint::Changed.
/// @param next The frame to show.
/// @param options What to cover and what to write besides cells.
CORE_TUI_CORE_API void writeFrameDiff(TerminalOutput& out,
                                      Buffer const& previous,
                                      Buffer const& next,
                                      DiffOptions const& options);

/// @brief Two buffers in, the bytes that turn the first into the second out; pure.
///
/// @c writeFrameDiff() into a string: no terminal, no state, the same bytes for the same input.
///
/// @param previous The frame on the screen.
/// @param next The frame to show.
/// @param options What to cover and what to write besides cells.
/// @return The escape sequences and text, empty when there is nothing to change.
[[nodiscard]] CORE_TUI_CORE_API std::string diffFrames(Buffer const& previous,
                                                       Buffer const& next,
                                                       DiffOptions options = {});

} // namespace core::tui
