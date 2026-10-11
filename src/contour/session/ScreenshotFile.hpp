// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/Screen.hpp>

#include <string>
#include <string_view>

namespace contour::session
{

/// What precedes a screenshot written to a file, so that the file replays on its own: whatever state the
/// terminal replaying it is in, and however large its page, it draws the page that was captured and
/// nothing else.
///
/// Screen::screenshot() itself starts wherever the replaying cursor stands, because the state dump prints
/// it among lines of its own; a file has no such surroundings. In order: the default rendition (SGR 0),
/// so that the erase below fills with the default background; the ASCII character set in G0, so that the
/// text is not drawn as line graphics; no origin mode (DECOM) and the full page as the scroll region
/// (DECSTBM), so that the home below is the page's top-left cell and the line breaks between rows move
/// down the page; then the cursor home (CUP) and the erase of the whole display (ED 2), which clears
/// what a larger page holds beyond the rows and columns the screenshot writes.
inline constexpr std::string_view StandaloneScreenshotPrefix = "\033[m\033(B\033[?6l\033[r\033[H\033[2J";

/// The VT stream the ScreenshotVT action writes to `screenshot.vt`.
/// @param screen The screen captured.
/// @return The screen's screenshot, preceded by StandaloneScreenshotPrefix.
[[nodiscard]] inline std::string standaloneScreenshot(vtbackend::Screen const& screen)
{
    return std::string(StandaloneScreenshotPrefix) + screen.screenshot();
}

} // namespace contour::session
