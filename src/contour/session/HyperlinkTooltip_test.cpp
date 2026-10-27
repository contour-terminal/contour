// SPDX-License-Identifier: Apache-2.0
//
// The pure decision layer behind the OSC 8 hyperlink tooltip: what text to show, and when to show or
// hide it. The tooltip itself is a QML popup and cannot be opened offscreen, so this is the seam that
// carries the behaviour under test.

#include <contour/session/HyperlinkTooltip.hpp>

#include <vtbackend/links/LinkIndex.hpp>
#include <vtbackend/links/LinkTypes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

using contour::session::elideMiddle;
using contour::session::HoverChange;
using contour::session::HyperlinkHoverTracker;
using contour::session::hyperlinkTooltipText;
using contour::session::OscLinkTarget;
using vtbackend::CellLocation;
using vtbackend::ColumnOffset;
using vtbackend::LineOffset;

namespace
{
/// The OSC 8 link @p uri, as the tracker is handed it; an empty URI is no link at all.
[[nodiscard]] contour::session::HoveredLink osc(std::string_view uri)
{
    if (uri.empty())
        return std::monostate {};
    return OscLinkTarget { .uri = std::string { uri } };
}

[[nodiscard]] CellLocation cell(int line, int column)
{
    return CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
}

/// The number of codepoints in @p text (its UTF-8 lead bytes).
[[nodiscard]] size_t codepointCount(std::string_view text)
{
    auto count = size_t { 0 };
    for (auto const ch: text)
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U)
            ++count;
    return count;
}
} // namespace

TEST_CASE("elideMiddle keeps both informative ends", "[contour][hyperlink]")
{
    SECTION("text that already fits is returned unchanged")
    {
        CHECK(elideMiddle("https://a.example/", 64) == "https://a.example/");
        // Exactly at the limit is still a fit.
        CHECK(elideMiddle("abcde", 5) == "abcde");
    }

    SECTION("a long URL keeps its scheme+host and its last path segment")
    {
        auto const url = std::string_view { "https://example.com/a/very/long/path/that/goes/on/final.txt" };
        auto const elided = elideMiddle(url, 30);

        CHECK(codepointCount(elided) == 30);
        CHECK(elided.starts_with("https://"));
        CHECK(elided.ends_with("final.txt"));
        CHECK(elided.contains("…"));
    }

    SECTION("a degenerate limit yields just the ellipsis rather than a broken string")
    {
        CHECK(elideMiddle("https://example.com/", 1) == "…");
        CHECK(elideMiddle("https://example.com/", 0) == "…");
    }
}

TEST_CASE("elideMiddle never splits a codepoint", "[contour][hyperlink]")
{
    // Internationalized URLs are real, and half a character rendered as a replacement glyph is worse
    // than eliding a little shorter. Counting bytes instead of codepoints is exactly how that happens.
    auto const url = std::string_view { "https://пример.example/каталог/файл-документа.txt" };

    for (auto const limit: { size_t { 5 }, size_t { 12 }, size_t { 20 }, size_t { 31 } })
    {
        auto const elided = elideMiddle(url, limit);
        CHECK(codepointCount(elided) <= limit);

        // Every byte of the result decodes: no lead byte is left without its continuations, and no
        // continuation byte stands alone at the start.
        CHECK((static_cast<unsigned char>(elided.front()) & 0xC0U) != 0x80U);
        auto roundTripped = std::string {};
        for (auto const ch: elided)
            roundTripped.push_back(ch);
        CHECK(roundTripped == elided);
    }
}

TEST_CASE("hyperlinkTooltipText says the useful part of a target", "[contour][hyperlink]")
{
    constexpr auto LocalHost = std::string_view { "fedora" };

    SECTION("no hyperlink means no tooltip")
    {
        CHECK(hyperlinkTooltipText("", LocalHost, 64).empty());
    }

    SECTION("a remote target is shown as written")
    {
        // The scheme and host are exactly what is worth reading before following a link.
        CHECK(hyperlinkTooltipText("https://example.com/x", LocalHost, 64) == "https://example.com/x");
        CHECK(hyperlinkTooltipText("mailto:someone@example.com", LocalHost, 64)
              == "mailto:someone@example.com");
    }

    SECTION("a local file is shown as a path, with its escapes decoded")
    {
        CHECK(hyperlinkTooltipText("file:///home/user/notes.txt", LocalHost, 64) == "/home/user/notes.txt");
        CHECK(hyperlinkTooltipText("file://localhost/tmp/x.log", LocalHost, 64) == "/tmp/x.log");
        CHECK(hyperlinkTooltipText("file:///home/user/my%20file.txt", LocalHost, 64)
              == "/home/user/my file.txt");
        // Once: an escaped '%' stays a '%', not the start of another escape.
        CHECK(hyperlinkTooltipText("file:///tmp/50%2541", LocalHost, 64) == "/tmp/50%41");
    }

    SECTION("what a decoded path or a target holds is shown, not obeyed")
    {
        // Decoded, %0A is a line feed and %E2%80%AE a right-to-left override: shown raw, the first would put
        // a line of the application's choosing under the tooltip, the second reverse what follows it.
        constexpr auto Fffd = std::string_view { "\xEF\xBF\xBD" };
        CHECK(hyperlinkTooltipText("file:///tmp/x%0AFake%1B", LocalHost, 64)
              == std::format("/tmp/x{0}Fake{0}", Fffd));
        CHECK(hyperlinkTooltipText("file:///tmp/%E2%80%AEtxt.exe", LocalHost, 64)
              == std::format("/tmp/{}txt.exe", Fffd));
        // U+202E spliced in byte by byte, rather than written into a literal, where it would be a misleading
        // bidi character itself.
        auto const rightToLeftOverride = std::string { '\xE2', '\x80', '\xAE' };
        CHECK(hyperlinkTooltipText("https://example.com/" + rightToLeftOverride + "gpj.exe", LocalHost, 64)
              == std::format("https://example.com/{}gpj.exe", Fffd));
    }

    SECTION("a query or fragment is no part of the path, which is the file a click opens")
    {
        CHECK(hyperlinkTooltipText("file:///tmp/x.log#L10", LocalHost, 64) == "/tmp/x.log");
        CHECK(hyperlinkTooltipText("file:///tmp/x.log?raw=1#top", LocalHost, 64) == "/tmp/x.log");
        // Escaped, either is part of a name.
        CHECK(hyperlinkTooltipText("file:///tmp/a%23b%3Fc", LocalHost, 64) == "/tmp/a#b?c");
    }

    SECTION("this machine's own name is a local path, however the application spelled it")
    {
        // The full set of accepted spellings is owned by vtbackend's FileUrl_test; these pin that the
        // tooltip is wired to it, rather than to the empty-or-"localhost" rule it used to apply.
        CHECK(hyperlinkTooltipText("file://fedora/tmp/x.log", LocalHost, 64) == "/tmp/x.log");
        CHECK(hyperlinkTooltipText("file://fedora.corp.example/tmp/x.log", LocalHost, 64) == "/tmp/x.log");
    }

    SECTION("a file on another host keeps its scheme, because the path alone would mislead")
    {
        CHECK(hyperlinkTooltipText("file://remote/tmp/x.log", LocalHost, 64) == "file://remote/tmp/x.log");
    }

    SECTION("a malformed escape is shown as written rather than swallowed")
    {
        CHECK(hyperlinkTooltipText("file:///tmp/a%zz", LocalHost, 64) == "/tmp/a%zz");
        CHECK(hyperlinkTooltipText("file:///tmp/trailing%", LocalHost, 64) == "/tmp/trailing%");
    }

    SECTION("where no file:// target names a path here, every one keeps its scheme")
    {
        // A pane replicating another machine's daemon: its application's paths are on that machine,
        // whatever authority it writes, so not even the empty one or "localhost" reads as a path here.
        CHECK(hyperlinkTooltipText("file:///home/user/notes.txt", std::nullopt, 64)
              == "file:///home/user/notes.txt");
        CHECK(hyperlinkTooltipText("file://localhost/tmp/x.log", std::nullopt, 64)
              == "file://localhost/tmp/x.log");
        CHECK(hyperlinkTooltipText("https://example.com/x", std::nullopt, 64) == "https://example.com/x");
    }
}

TEST_CASE("linkPillText reads a URL as a breadcrumb", "[contour][hyperlink]")
{
    using contour::session::linkPillText;
    constexpr auto LocalHost = std::string_view { "fedora" };
    constexpr auto Max = size_t { 100 };

    // U+203A between spaces.
    CHECK(linkPillText("https://github.com/contour-terminal/contour/issues/2175", LocalHost, Max)
          == "github.com \u203A contour-terminal \u203A contour \u203A issues \u203A 2175");
    // The scheme, user info, port, query and fragment go; empty segments are skipped.
    CHECK(linkPillText("https://user@host:8443/a//b/?q=1#f", LocalHost, Max) == "host \u203A a \u203A b");
    CHECK(linkPillText("https://example.com", LocalHost, Max) == "example.com");
    CHECK(linkPillText("https://example.com/", LocalHost, Max) == "example.com");
    // A segment is read decoded.
    CHECK(linkPillText("https://example.com/my%20page", LocalHost, Max) == "example.com \u203A my page");
    // A local file is its decoded path.
    CHECK(linkPillText("file:///home/user/my%20file.txt", LocalHost, Max) == "/home/user/my file.txt");
    // A file elsewhere keeps its scheme: the path alone would mislead.
    CHECK(linkPillText("file://remote/tmp/x.log", LocalHost, Max) == "file://remote/tmp/x.log");
    CHECK(linkPillText("file:///tmp/x.log", std::nullopt, Max) == "file:///tmp/x.log");
    // Anything that is no URL with an authority, as written.
    CHECK(linkPillText("mailto:someone@example.com", LocalHost, Max) == "mailto:someone@example.com");
    // A "://" that does not follow the scheme is text, not an authority: read as one, these would show a host
    // the link does not open, and hide the scheme that says what it does.
    CHECK(linkPillText("mailto:x@y.z?body=https://github.com/contour", LocalHost, Max)
          == "mailto:x@y.z?body=https://github.com/contour");
    CHECK(linkPillText("data:text/html,https://example.com/login", LocalHost, Max)
          == "data:text/html,https://example.com/login");
    CHECK(linkPillText("://example.com/x", LocalHost, Max) == "://example.com/x");
    CHECK(linkPillText("1http://example.com/x", LocalHost, Max) == "1http://example.com/x");
    CHECK(linkPillText("git+ssh://host/repo", LocalHost, Max) == "host \u203A repo");
    // A file elsewhere keeps its scheme however the scheme is spelled.
    CHECK(linkPillText("FILE://remote/tmp/x.log", LocalHost, Max) == "FILE://remote/tmp/x.log");
    // And a local one is its path however its scheme is spelled.
    CHECK(linkPillText("FILE:///home/user/my%20file.txt", LocalHost, Max) == "/home/user/my file.txt");
    CHECK(linkPillText("File://fedora/tmp/x.log", LocalHost, Max) == "/tmp/x.log");
    CHECK(linkPillText("", LocalHost, Max).empty());

    // What it holds is shown, not obeyed: U+202E spliced in byte by byte.
    auto const rightToLeftOverride = std::string { '\xE2', '\x80', '\xAE' };
    constexpr auto Fffd = std::string_view { "\xEF\xBF\xBD" };
    CHECK(linkPillText("https://example.com/" + rightToLeftOverride + "gpj.exe", LocalHost, Max)
          == std::format("example.com \u203A {}gpj.exe", Fffd));
    CHECK(linkPillText("https://example.com/%E2%80%AEgpj.exe", LocalHost, Max)
          == std::format("example.com \u203A {}gpj.exe", Fffd));

    // One absurd URL is capped.
    auto const huge = "https://example.com/" + std::string(300, 'x');
    CHECK(codepointCount(linkPillText(huge, LocalHost, Max)) <= Max);
}

TEST_CASE("linkPillText names a detected link's target even where it is printed",
          "[contour][hyperlink][links]")
{
    using contour::session::linkPillText;
    using vtbackend::DetectedLink;
    using vtbackend::LinkKind;
    using vtbackend::LinkLocation;
    using vtbackend::LinkTarget;
    using vtbackend::PathKind;
    constexpr auto Max = size_t { 100 };
    auto const link = [](std::string text, LinkTarget target) {
        return DetectedLink { .id = {}, .target = std::move(target), .text = std::move(text), .spans = {} };
    };

    // A URL printed as it opens: the pill names it all the same, as a breadcrumb.
    CHECK(linkPillText(link("https://x.y/a/b",
                            LinkTarget { .kind = LinkKind::Url,
                                         .target = "https://x.y/a/b",
                                         .location = std::nullopt,
                                         .pathKind = PathKind::Missing,
                                         .directory = {} }),
                       Max)
          == "x.y \u203A a \u203A b");
    CHECK(linkPillText(link("src/a.cpp:3:1",
                            LinkTarget { .kind = LinkKind::Path,
                                         .target = "/w/src/a.cpp",
                                         .location = LinkLocation { .line = 3, .column = 1 },
                                         .pathKind = PathKind::File,
                                         .directory = "/w" }),
                       Max)
          == "/w/src/a.cpp:3:1");
    CHECK(linkPillText(link("/w/b.txt",
                            LinkTarget { .kind = LinkKind::Path,
                                         .target = "/w/b.txt",
                                         .location = std::nullopt,
                                         .pathKind = PathKind::File,
                                         .directory = {} }),
                       Max)
          == "/w/b.txt");
    CHECK(linkPillText(link("ABC-1",
                            LinkTarget { .kind = LinkKind::Custom,
                                         .target = "https://jira/browse/ABC-1",
                                         .location = std::nullopt,
                                         .pathKind = PathKind::Missing,
                                         .directory = {} }),
                       Max)
          == "jira \u203A browse \u203A ABC-1");
}

TEST_CASE("linkPillVisibility shows the pill where a click would follow a named target",
          "[contour][hyperlink]")
{
    using contour::session::LinkPillVisibility;
    using contour::session::LinkTargetReveal;
    using vtbackend::FollowModifiers;
    using vtbackend::LinkPointer;
    constexpr auto Targets = std::array<std::string_view, 2> { "", "x" };

    auto shown = 0;
    for (auto const reveal: std::views::iota(size_t { 0 }, contour::session::LinkTargetRevealCount))
        for (auto const pointer: std::views::iota(size_t { 0 }, vtbackend::LinkPointerCount))
            for (auto const held: std::views::iota(size_t { 0 }, vtbackend::FollowModifiersCount))
                for (auto const target: Targets)
                {
                    auto const r = static_cast<LinkTargetReveal>(reveal);
                    auto const p = static_cast<LinkPointer>(pointer);
                    auto const h = static_cast<FollowModifiers>(held);
                    auto const expected =
                        !target.empty() && p == LinkPointer::Over
                                && (h == FollowModifiers::Held || r == LinkTargetReveal::Hover)
                            ? LinkPillVisibility::Shown
                            : LinkPillVisibility::Hidden;
                    INFO("reveal " << reveal << ", pointer " << pointer << ", held " << held << ", target '"
                                   << target << "'");
                    CHECK(contour::session::linkPillVisibility(r, p, h, target) == expected);
                    shown += expected == LinkPillVisibility::Shown ? 1 : 0;
                }
    // Over a target with a click following it: shown whenever held, and on plain hover where asked.
    CHECK(shown == 3);

    CHECK(contour::session::linkTargetRevealOf(true) == LinkTargetReveal::Hover);
    CHECK(contour::session::linkTargetRevealOf(false) == LinkTargetReveal::HeldModifier);
}

TEST_CASE("HyperlinkHoverTracker carries a detected link's text as it is handed it", "[contour][hyperlink]")
{
    // A detected link's pill text is formatted already (linkPillText(DetectedLink)): read as a URI again, a
    // path would become a breadcrumb of a breadcrumb.
    auto tracker = HyperlinkHoverTracker { [] { return std::optional<std::string_view> { "fedora" }; } };
    auto const entered =
        tracker.update(contour::session::DetectedLinkText { .text = "x.y \u203A a" }, cell(1, 2), 64);
    CHECK(entered.change == HoverChange::Changed);
    CHECK(entered.text == "x.y \u203A a");
    // The same text again is the same link.
    CHECK(tracker.update(contour::session::DetectedLinkText { .text = "x.y \u203A a" }, cell(1, 3), 64).change
          == HoverChange::Unchanged);
    // An OSC 8 link spelled like it is another link.
    CHECK(tracker.update(osc("x.y \u203A a"), cell(1, 4), 64).change == HoverChange::Changed);
}

TEST_CASE("HyperlinkHoverTracker shows a detected link's text safe and short", "[contour][hyperlink]")
{
    // Formatted already, but a path can hold anything a program printed: shown, never obeyed, and capped.
    auto tracker = HyperlinkHoverTracker { [] { return std::optional<std::string_view> { "fedora" }; } };
    auto const rightToLeftOverride = std::string { '\xE2', '\x80', '\xAE' };
    auto const spoofed = tracker.update(
        contour::session::DetectedLinkText { .text = "/tmp/" + rightToLeftOverride + "txt.exe" },
        cell(0, 0),
        64);
    CHECK(spoofed.text == "/tmp/\xEF\xBF\xBDtxt.exe");

    auto const lengthy = tracker.update(
        contour::session::DetectedLinkText { .text = "/" + std::string(300, 'x') }, cell(1, 0), 40);
    CHECK(codepointCount(lengthy.text) == 40);
}

TEST_CASE("HyperlinkHoverTracker asks where a file:// target is local at each link it enters",
          "[contour][hyperlink]")
{
    // A pane becomes a replica of another machine's terminal after it is built, so the tracker asks rather
    // than being told once.
    auto localHost = std::optional<std::string_view> { "fedora" };
    auto tracker = HyperlinkHoverTracker { [&] { return localHost; } };

    CHECK(tracker.update(osc("file:///tmp/x.log"), cell(0, 0), 64).text == "/tmp/x.log");
    CHECK(tracker.clear().change == HoverChange::Changed);

    localHost = std::nullopt;
    CHECK(tracker.update(osc("file:///tmp/x.log"), cell(0, 0), 64).text == "file:///tmp/x.log");
}

TEST_CASE("HyperlinkHoverTracker announces only real transitions", "[contour][hyperlink]")
{
    auto tracker = HyperlinkHoverTracker { [] { return std::optional<std::string_view> { "fedora" }; } };

    SECTION("entering a link announces it, anchored where it was entered")
    {
        auto const entered = tracker.update(osc("https://example.com/"), cell(3, 10), 64);
        CHECK(entered.change == HoverChange::Changed);
        CHECK(entered.text == "example.com");
        CHECK(entered.anchor == cell(3, 10));
    }

    SECTION("moving within one link announces nothing")
    {
        CHECK(tracker.update(osc("https://example.com/"), cell(3, 10), 64).change == HoverChange::Changed);

        // The whole reason this holds state: a tooltip has a show delay, and re-announcing on every
        // cell would restart it, so a tooltip over a link being slowly traced would never appear.
        for (auto const column: { 11, 12, 13, 14 })
            CHECK_FALSE(tracker.update(osc("https://example.com/"), cell(3, column), 64).change
                        == HoverChange::Changed);
    }

    SECTION("crossing to a different link re-anchors")
    {
        CHECK(tracker.update(osc("https://a.example/"), cell(1, 1), 64).change == HoverChange::Changed);

        auto const crossed = tracker.update(osc("https://b.example/"), cell(1, 40), 64);
        CHECK(crossed.change == HoverChange::Changed);
        CHECK(crossed.text == "b.example");
        CHECK(crossed.anchor == cell(1, 40));
    }

    SECTION("leaving a link hides the tooltip")
    {
        CHECK(tracker.update(osc("https://example.com/"), cell(3, 10), 64).change == HoverChange::Changed);

        auto const left = tracker.update(osc(""), cell(3, 40), 64);
        CHECK(left.change == HoverChange::Changed);
        CHECK(left.text.empty());
    }

    SECTION("nothing is announced while there was nothing to hide")
    {
        // Both a bare clear() and a move over plain text must stay silent, or every mouse move over an
        // ordinary terminal would push an update to the GUI.
        CHECK_FALSE(tracker.clear().change == HoverChange::Changed);
        CHECK_FALSE(tracker.update(osc(""), cell(0, 0), 64).change == HoverChange::Changed);
        CHECK_FALSE(tracker.update(osc(""), cell(0, 1), 64).change == HoverChange::Changed);
    }

    SECTION("clear() hides, and a second clear() stays silent")
    {
        CHECK(tracker.update(osc("https://example.com/"), cell(2, 2), 64).change == HoverChange::Changed);
        CHECK(tracker.clear().change == HoverChange::Changed);
        CHECK_FALSE(tracker.clear().change == HoverChange::Changed);
    }

    SECTION("re-entering the same link after leaving announces it again")
    {
        CHECK(tracker.update(osc("https://example.com/"), cell(2, 2), 64).change == HoverChange::Changed);
        CHECK(tracker.clear().change == HoverChange::Changed);
        // The tooltip was hidden, so the same URL is a real transition once more -- a tracker that
        // remembered it would leave the user with no tooltip on the way back.
        CHECK(tracker.update(osc("https://example.com/"), cell(2, 2), 64).change == HoverChange::Changed);
    }

    SECTION("the shown text is elided, so one absurd URL cannot span the window")
    {
        auto const huge = std::string("https://example.com/") + std::string(500, 'x');
        auto const entered = tracker.update(osc(huge), cell(0, 0), 40);
        CHECK(entered.change == HoverChange::Changed);
        CHECK(codepointCount(entered.text) == 40);
    }
}
