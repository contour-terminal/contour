// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/App.hpp>
#include <crispy/Times.hpp>
#include <crispy/Utils.hpp>
#include <crispy/testing/Environment.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/// Helpers shared by the Terminal_*_test.cpp translation units.
///
/// Terminal_test.cpp was one file of several thousand lines; splitting it by topic left these
/// with more than one consumer, so they live here rather than being copied into each part.
/// They carry external linkage deliberately: an unused internal-linkage helper would be a
/// -Werror break in whichever part does not happen to use it.
namespace vtbackend::test
{

// TODO: Test case possibilities:
//
// - [x] Synchronized output (?2026)
// - [x] Blinking cursor visibility over time and on input events
// - [ ] double click word selection
// - [ ] triple click line selection
// - [ ] rectangular block selection
// - [ ] text selection with bypassing enabled application mouse protocol
// - [ ] extractLastMarkRange
// - [ ] scroll mark up
// - [ ] scroll mark down

// TODO: Writing text, leading to page-scroll properly updates viewport.
// TODO: Writing text, leading to page-scroll properly updates active selection.

/// Total scroll displacement in pixels: whole-line offset plus the sub-cell pixel remainder.
[[nodiscard]] float totalScrollPixels(vtbackend::Terminal const& terminal, float cellHeight) noexcept;

/// Background color of screen @p line at a column away from column 0, in a freshly built render
/// buffer. Sampling off column 0 avoids the block-cursor cell (which inverts fg/bg) when the
/// cursor sits at the line start. A line is emitted either as per-cell RenderCells (the
/// per-cell/fallback path) or as one batched RenderLine (the trivial simple path); check both.
vtbackend::RGBColor screenLineBackground(vtbackend::RenderBufferRef const& buf,
                                         vtbackend::LineOffset line) noexcept;

/// Screen line the cursor is rendered on, per the render buffer itself — the single source of
/// truth for which line should carry the cursorline highlight (the render loop and the highlight
/// decision share this coordinate). Returns nullopt when the buffer carries no cursor.
std::optional<vtbackend::LineOffset> renderedCursorLine(vtbackend::RenderBufferRef const& buf) noexcept;

/// Which representation a line reached the render buffer through.
enum class RenderPath : uint8_t
{
    /// Per-cell RenderCells. Taken by a line the batched form cannot express, and by any line
    /// carrying a selection, a cursor or a highlight.
    PerCell,
    /// One batched RenderLine covering the whole line.
    Batched,
};

/// One entry per drawn glyph on screen line @p line: the column it lands on, its text, and how many
/// columns it covers.
///
/// Derived from whichever representation the render buffer used, so a batched line and a per-cell
/// line are directly comparable -- which is the whole point, since a selection is what switches
/// between them. Continuation and blank cells are skipped: they carry no glyph of their own.
struct DrawnGlyph
{
    int column;
    std::u32string codepoints;
    int width;

    bool operator==(DrawnGlyph const&) const = default;
};

std::vector<DrawnGlyph> renderedLayout(vtbackend::RenderBufferRef const& buf, vtbackend::LineOffset line);

/// A layout as `col:U+XXXX+U+YYYY(wN) ...`. Catch2 prints a u32string as `{?}`, which says nothing
/// about a mismatch that is entirely about which codepoints landed where, and how wide.
std::string describeLayout(std::vector<DrawnGlyph> const& layout);

/// The layout of screen line @p line as rendered right now, plus which path produced it.
/// Snapshotted so the render buffer's lock is released before the caller changes the terminal.
std::pair<std::string, RenderPath> renderLineOf(vtbackend::Terminal& terminal, LineOffset line);

/// Selects columns [@p firstColumn, @p lastColumn] of screen line @p line, as a mouse drag would.
void selectColumns(vtbackend::Terminal& terminal,
                   LineOffset line,
                   ColumnOffset firstColumn,
                   ColumnOffset lastColumn);

} // namespace vtbackend::test
