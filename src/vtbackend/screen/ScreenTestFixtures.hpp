// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/Viewport.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <crispy/Escape.hpp>
#include <crispy/Utils.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>

/// Helpers shared by the Screen_*_test.cpp translation units.
///
/// Screen_test.cpp was one file of several thousand lines; splitting it by topic left these
/// with more than one consumer, so they live here rather than being copied into each part.
/// They carry external linkage deliberately: an unused internal-linkage helper would be a
/// -Werror break in whichever part does not happen to use it.
namespace vtbackend::test
{

// Chessboard image with each square of size 10x10 pixels
inline constexpr auto ChessBoard = std::string_view {
    R"=(P0;0;0q"1;1;100;100#0;2;0;0;0#1;2;100;100;100#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-#0!10N!10o!10N!10o!10N!10o!10N!10o!10N!10o$#1!10o!10N!10o!10N!10o!10N!10o!10N!10o!10N-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10{!10B!10{!10B!10{!10B!10{!10B!10{!10B$#1!10B!10{!10B!10{!10B!10{!10B!10{!10B!10{-#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10o!10N!10o!10N!10o!10N!10o!10N!10o!10N$#1!10N!10o!10N!10o!10N!10o!10N!10o!10N!10o-#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-#0!10B!10{!10B!10{!10B!10{!10B!10{!10B!10{$#1!10{!10B!10{!10B!10{!10B!10{!10B!10{!10B-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-#0!10N!10o!10N!10o!10N!10o!10N!10o!10N!10o$#1!10o!10N!10o!10N!10o!10N!10o!10N!10o!10N-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-!10{!10B!10{!10B!10{!10B!10{!10B!10{!10B$#1!10B!10{!10B!10{!10B!10{!10B!10{!10B!10{-#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~-!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~#1!10~#0!10~-#1!10N#0!10N#1!10N#0!10N#1!10N#0!10N#1!10N#0!10N#1!10N#0!10N-\)="
};

/// Opaque black 10x10 RGBA pixels.
Image::Data const& black10x10();

/// Opaque white 10x10 RGBA pixels.
Image::Data const& white10x10();

struct TextRenderBuilder
{
    std::string text;

    void startLine(LineOffset lineOffset, LineFlags flags);
    void renderCell(ConstCellProxy cell, LineOffset lineOffset, ColumnOffset columnOffset);
    void endLine();
    void renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                           LineOffset lineOffset,
                           LineFlags flags,
                           std::u32string_view textOverride = {});
    void finish();
};

MockTerm<vtpty::MockPty> screenForDECRA();

// }}}

// {{{ writeText
// AutoWrap disabled: text length is less then available columns in line.
/// Parses a DA1 response string (e.g. "\033[?65;1;4;6;...c") and returns the set of extension numbers.
std::set<int> parseDA1Extensions(std::string_view reply);

/// Parses the conformance level from a DA1 response.
int parseDA1Level(std::string_view reply);

/// Drains the grid's pending changes so a test observes only its own writes.
GridDeltaCursor drainedDeltaCursor(Grid& grid);

std::vector<int> changedLineOffsets(Grid& grid, GridDeltaCursor& cursor);

} // namespace vtbackend::test
