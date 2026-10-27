// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/LinkPresentation.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace vtbackend
{
struct DetectedLink;
} // namespace vtbackend

namespace contour::session
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
/// A local file:// URI shows its decoded path, because the scheme and the authority naming this very
/// machine are noise to someone looking at a path they may want to open. Everything else shows the URI as
/// written: for a remote target the scheme and host are exactly the part worth reading before clicking.
/// Either is made safe to show (vtbackend::SanitizePurpose::Display), so a control or bidi override it
/// holds is replaced rather than obeyed.
///
/// @param uri       The hyperlink's target, as the application set it.
/// @param localHost This machine's host name, against which the URI's authority is measured; nullopt when
///                  no file:// URI names a path here -- a pane replicating another machine's daemon
///                  (vtbackend::Terminal::fileUrlLocalHost).
/// @param maxLength Maximum number of codepoints to show.
/// @return The tooltip text, or empty when there is nothing worth showing.
[[nodiscard]] std::string hyperlinkTooltipText(std::string_view uri,
                                               std::optional<std::string_view> localHost,
                                               size_t maxLength);

/// What the link pill says for an OSC 8 target (spec R-f).
///
/// A URL with an authority reads as its host and its non-empty, decoded path segments joined by " › "
/// (U+203A between spaces): "github.com › contour-terminal › contour". Its scheme, user info, port, query and
/// fragment are dropped. A local file:// URI reads as its decoded path; one naming another host keeps its
/// scheme, since the path alone would mislead. Anything else (mailto:, a bare word) reads as written. Made
/// safe to show (vtbackend::SanitizePurpose::Display) and middle-elided to @p maxLength codepoints.
/// @param uri       The hyperlink's target, as the application set it.
/// @param localHost This machine's host name; nullopt where no file:// URI names a path here.
/// @param maxLength Maximum number of codepoints to show.
/// @return The text; empty for an empty @p uri.
[[nodiscard]] std::string linkPillText(std::string_view uri,
                                       std::optional<std::string_view> localHost,
                                       size_t maxLength);

/// What the link pill says for a detected link: always its target, even where it is printed as it opens.
/// A URL reads as linkPillText(std::string_view, …) reads it; a path as its absolute form with
/// `:line:column` where detection found one.
/// @param link      The link.
/// @param maxLength Maximum number of codepoints to show.
/// @return The text; never empty for a link with a target.
[[nodiscard]] std::string linkPillText(vtbackend::DetectedLink const& link, size_t maxLength);

/// When a link's target shows (`hyperlink_hover_tooltip`, converted at the boundary by linkTargetRevealOf()).
enum class LinkTargetReveal : uint8_t
{
    HeldModifier = 0, ///< Only while the follow modifier is held.
    Hover,            ///< On plain hover as well.
};

/// How many LinkTargetReveal values there are, derived from the last one.
inline constexpr size_t LinkTargetRevealCount = static_cast<size_t>(LinkTargetReveal::Hover) + 1;

/// Converts the YAML boolean `hyperlink_hover_tooltip` at the boundary.
/// @param hoverTooltip The configured value (Config's documented `bool` carve-out).
/// @return LinkTargetReveal::Hover for true, LinkTargetReveal::HeldModifier for false.
[[nodiscard]] constexpr LinkTargetReveal linkTargetRevealOf(bool hoverTooltip) noexcept
{
    return hoverTooltip ? LinkTargetReveal::Hover : LinkTargetReveal::HeldModifier;
}

/// Whether the link pill shows.
enum class LinkPillVisibility : uint8_t
{
    Hidden = 0, ///< Not shown.
    Shown,      ///< Shown.
};

/// Whether the link pill shows for the link under the pointer.
///
/// Only where a click would follow the link (@p pointer is Over): while an application takes the clicks, a
/// pill inviting one would lie. There, the target shows while the follow modifier is held, or on plain hover
/// where @p reveal asks for it.
/// @param reveal  When the target shows.
/// @param pointer Whether a click on the link would follow it (vtbackend::Terminal::hoverPointer()).
/// @param held    Whether the follow modifier is held.
/// @param target  The pill's text (linkPillText()); empty over no link.
/// @return LinkPillVisibility::Shown when there is a target to show and the rule above allows it.
[[nodiscard]] constexpr LinkPillVisibility linkPillVisibility(LinkTargetReveal reveal,
                                                              vtbackend::LinkPointer pointer,
                                                              vtbackend::FollowModifiers held,
                                                              std::string_view target) noexcept
{
    auto const revealed = held == vtbackend::FollowModifiers::Held || reveal == LinkTargetReveal::Hover;
    return !target.empty() && pointer == vtbackend::LinkPointer::Over && revealed
               ? LinkPillVisibility::Shown
               : LinkPillVisibility::Hidden;
}

/// An OSC 8 hyperlink under the pointer: its target as the application set it.
struct OscLinkTarget
{
    std::string uri; ///< The target.

    [[nodiscard]] bool operator==(OscLinkTarget const&) const = default;
};

/// A detected link under the pointer: its pill text, formatted already (linkPillText(DetectedLink, …)).
struct DetectedLinkText
{
    std::string text; ///< What the pill says.

    [[nodiscard]] bool operator==(DetectedLinkText const&) const = default;
};

/// The link under the pointer, if any. Kept apart by kind so a detected link's formatted text is never read
/// as a URI again.
using HoveredLink = std::variant<std::monostate, OscLinkTarget, DetectedLinkText>;

/// Whether a hover update has anything to announce.
enum class HoverChange : uint8_t
{
    Unchanged = 0, ///< Nothing to announce.
    Changed,       ///< The link under the pointer, and so its text, changed.
};

/// Tracks which hyperlink the pointer is over, and says when a tooltip should appear or vanish.
///
/// Kept free of Qt, of the terminal and of the display so the decision is testable on its own — the
/// tooltip itself cannot be, because a QML popup has no overlay to open into offscreen.
class HyperlinkHoverTracker
{
  public:
    /// Says, when asked, the host name a file:// target must name to be a path on this machine, or nullopt
    /// when none does. @see hyperlinkTooltipText
    using LocalHostSource = std::function<std::optional<std::string_view>()>;

    /// @param localHost Asked at each link the pointer enters, telling a local file:// target from a remote
    ///        one -- asked, not told once, as a pane becomes a replica of another machine's terminal only
    ///        after it is built (vtbackend::Terminal::fileUrlLocalHost).
    explicit HyperlinkHoverTracker(LocalHostSource localHost): _localHost { std::move(localHost) } {}

    /// What the GUI should do about the tooltip, if anything.
    struct Change
    {
        HoverChange change = HoverChange::Unchanged; ///< Whether anything need be announced at all.
        std::string text;                            ///< What to show (linkPillText()); empty means hide.
        vtbackend::CellLocation anchor {};           ///< The cell the pointer ENTERED the link at.
    };

    /// Reports the hyperlink under the pointer.
    ///
    /// Moving WITHIN one link reports no change. That is the point of holding state here: a tooltip has
    /// a show delay, and re-announcing on every cell would restart it on each one, so a tooltip over a
    /// link the user is slowly tracing would never actually appear.
    ///
    /// @param link      The link under the pointer; std::monostate over none.
    /// @param cell      Where the pointer is, in viewport coordinates.
    /// @param maxLength Maximum number of codepoints an OSC 8 target's text shows; a detected link's text is
    ///                  shown as handed in.
    /// @return What changed, if anything.
    [[nodiscard]] Change update(HoveredLink const& link, vtbackend::CellLocation cell, size_t maxLength);

    /// Reports that there is no longer a hyperlink under the pointer -- it left the terminal, or the
    /// view scrolled out from under it.
    ///
    /// @return What changed; nothing when no tooltip was showing.
    [[nodiscard]] Change clear();

  private:
    LocalHostSource _localHost;
    HoveredLink _link {};
    vtbackend::CellLocation _anchor {};
};

} // namespace contour::session
