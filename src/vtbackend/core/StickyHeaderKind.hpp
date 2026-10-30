// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// What the sticky header shows.
///
/// In core/ rather than shell/StickyHeader.hpp because the render buffer that carries the header and the
/// renderer that draws it name it too, and render/ may include nothing from shell/. shell/StickyHeader.hpp
/// includes this header, so every caller of the decision sees it under the same name.
enum class StickyHeaderKind : uint8_t
{
    Command = 0, ///< The block's input row, copied from the grid with its own colours.
    Evicted,     ///< `⋯ <command> — earlier output evicted`: the head has left the scrollback.
};

/// Where the sticky header is drawn: over the page's top row, or in the window's chrome beside the page.
///
/// In core/ for StickyHeaderKind's reason: the render buffer carries it and the renderer reads it.
/// Not a profile key: the pane's card decides it from its header policy (window::stickyHeaderHostFor), and
/// the session hands it to the terminal. The decision of WHETHER a header shows is the same either way.
enum class StickyHeaderPresentation : uint8_t
{
    InGrid = 0, ///< A band over the main page's first row, drawn by the renderer with the cells.
    /// In the pane header or the title band: the frame still carries the header, as data for the chrome,
    /// but the renderer draws nothing and the top row stays the grid's, to press and to read.
    Chrome,
};

} // namespace vtbackend
