// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace contour
{

/// Middle-elides @p text to at most @p maxLength codepoints.
///
/// Middle rather than tail, because a URL's two informative ends are its scheme-and-host and its final
/// path segment; eliding the tail of a long link leaves every candidate looking identical.
///
/// Counts CODEPOINTS, not bytes, and never cuts one in half: an internationalized URL rendered with half
/// a character in it is worse than one elided a little shorter.
///
/// @param text      The text to shorten.
/// @param maxLength Maximum number of codepoints in the result, including the ellipsis.
/// @return @p text unchanged when it already fits, otherwise the elided form.
[[nodiscard]] std::string elideMiddle(std::string_view text, size_t maxLength);

/// What to show for a hovered OSC 8 target, or empty for "say nothing".
///
/// A local file:// URI shows its decoded path, because the scheme and an empty authority are noise to
/// someone looking at a path they may want to open. Everything else shows the URI as written: for a
/// remote target the scheme and host are exactly the part worth reading before clicking.
///
/// @param uri       The hyperlink's target, as the application set it.
/// @param maxLength Maximum number of codepoints to show.
/// @return The tooltip text, or empty when there is nothing worth showing.
[[nodiscard]] std::string hyperlinkTooltipText(std::string_view uri, size_t maxLength);

/// Tracks which hyperlink the pointer is over, and says when a tooltip should appear or vanish.
///
/// Kept free of Qt, of the terminal and of the display so the decision is testable on its own — the
/// tooltip itself cannot be, because a QML popup has no overlay to open into offscreen.
class HyperlinkHoverTracker
{
  public:
    /// What the GUI should do about the tooltip, if anything.
    struct Change
    {
        bool changed = false;              ///< Whether anything need be announced at all.
        std::string text;                  ///< What to show; empty means hide.
        vtbackend::CellLocation anchor {}; ///< The cell the pointer ENTERED the link at.
    };

    /// Reports the hyperlink under the pointer.
    ///
    /// Moving WITHIN one link reports no change. That is the point of holding state here: a tooltip has
    /// a show delay, and re-announcing on every cell would restart it on each one, so a tooltip over a
    /// link the user is slowly tracing would never actually appear.
    ///
    /// @param uri       The hyperlink under the pointer, or empty when there is none.
    /// @param cell      Where the pointer is, in viewport coordinates.
    /// @param maxLength Maximum number of codepoints to show.
    /// @return What changed, if anything.
    [[nodiscard]] Change update(std::string_view uri, vtbackend::CellLocation cell, size_t maxLength);

    /// Reports that there is no longer a hyperlink under the pointer -- it left the terminal, or the
    /// view scrolled out from under it.
    ///
    /// @return What changed; nothing when no tooltip was showing.
    [[nodiscard]] Change clear();

  private:
    std::string _uri;
    vtbackend::CellLocation _anchor {};
};

} // namespace contour
