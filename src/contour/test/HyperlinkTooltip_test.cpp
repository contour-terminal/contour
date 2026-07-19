// SPDX-License-Identifier: Apache-2.0
//
// The pure decision layer behind the OSC 8 hyperlink tooltip: what text to show, and when to show or
// hide it. The tooltip itself is a QML popup and cannot be opened offscreen, so this is the seam that
// carries the behaviour under test.

#include <contour/HyperlinkTooltip.h>

#include <catch2/catch_test_macros.hpp>

using contour::elideMiddle;
using contour::HyperlinkHoverTracker;
using contour::hyperlinkTooltipText;
using vtbackend::CellLocation;
using vtbackend::ColumnOffset;
using vtbackend::LineOffset;

namespace
{
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
        CHECK(elided.find("…") != std::string::npos);
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
        auto reencoded = std::string {};
        for (auto const ch: elided)
            reencoded.push_back(ch);
        CHECK(reencoded == elided);
    }
}

TEST_CASE("hyperlinkTooltipText says the useful part of a target", "[contour][hyperlink]")
{
    SECTION("no hyperlink means no tooltip")
    {
        CHECK(hyperlinkTooltipText("", 64).empty());
    }

    SECTION("a remote target is shown as written")
    {
        // The scheme and host are exactly what is worth reading before following a link.
        CHECK(hyperlinkTooltipText("https://example.com/x", 64) == "https://example.com/x");
        CHECK(hyperlinkTooltipText("mailto:someone@example.com", 64) == "mailto:someone@example.com");
    }

    SECTION("a local file is shown as a path, with its escapes decoded")
    {
        CHECK(hyperlinkTooltipText("file:///home/user/notes.txt", 64) == "/home/user/notes.txt");
        CHECK(hyperlinkTooltipText("file://localhost/tmp/x.log", 64) == "/tmp/x.log");
        CHECK(hyperlinkTooltipText("file:///home/user/my%20file.txt", 64) == "/home/user/my file.txt");
    }

    SECTION("a file on another host keeps its scheme, because the path alone would mislead")
    {
        CHECK(hyperlinkTooltipText("file://otherhost/tmp/x.log", 64) == "file://otherhost/tmp/x.log");
    }

    SECTION("a malformed escape is shown as written rather than swallowed")
    {
        CHECK(hyperlinkTooltipText("file:///tmp/a%zz", 64) == "/tmp/a%zz");
        CHECK(hyperlinkTooltipText("file:///tmp/trailing%", 64) == "/tmp/trailing%");
    }
}

TEST_CASE("HyperlinkHoverTracker announces only real transitions", "[contour][hyperlink]")
{
    auto tracker = HyperlinkHoverTracker {};

    SECTION("entering a link announces it, anchored where it was entered")
    {
        auto const entered = tracker.update("https://example.com/", cell(3, 10), 64);
        CHECK(entered.changed);
        CHECK(entered.text == "https://example.com/");
        CHECK(entered.anchor == cell(3, 10));
    }

    SECTION("moving within one link announces nothing")
    {
        CHECK(tracker.update("https://example.com/", cell(3, 10), 64).changed);

        // The whole reason this holds state: a tooltip has a show delay, and re-announcing on every
        // cell would restart it, so a tooltip over a link being slowly traced would never appear.
        for (auto const column: { 11, 12, 13, 14 })
            CHECK_FALSE(tracker.update("https://example.com/", cell(3, column), 64).changed);
    }

    SECTION("crossing to a different link re-anchors")
    {
        CHECK(tracker.update("https://a.example/", cell(1, 1), 64).changed);

        auto const crossed = tracker.update("https://b.example/", cell(1, 40), 64);
        CHECK(crossed.changed);
        CHECK(crossed.text == "https://b.example/");
        CHECK(crossed.anchor == cell(1, 40));
    }

    SECTION("leaving a link hides the tooltip")
    {
        CHECK(tracker.update("https://example.com/", cell(3, 10), 64).changed);

        auto const left = tracker.update("", cell(3, 40), 64);
        CHECK(left.changed);
        CHECK(left.text.empty());
    }

    SECTION("nothing is announced while there was nothing to hide")
    {
        // Both a bare clear() and a move over plain text must stay silent, or every mouse move over an
        // ordinary terminal would push an update to the GUI.
        CHECK_FALSE(tracker.clear().changed);
        CHECK_FALSE(tracker.update("", cell(0, 0), 64).changed);
        CHECK_FALSE(tracker.update("", cell(0, 1), 64).changed);
    }

    SECTION("clear() hides, and a second clear() stays silent")
    {
        CHECK(tracker.update("https://example.com/", cell(2, 2), 64).changed);
        CHECK(tracker.clear().changed);
        CHECK_FALSE(tracker.clear().changed);
    }

    SECTION("re-entering the same link after leaving announces it again")
    {
        CHECK(tracker.update("https://example.com/", cell(2, 2), 64).changed);
        CHECK(tracker.clear().changed);
        // The tooltip was hidden, so the same URL is a real transition once more -- a tracker that
        // remembered it would leave the user with no tooltip on the way back.
        CHECK(tracker.update("https://example.com/", cell(2, 2), 64).changed);
    }

    SECTION("the shown text is elided, so one absurd URL cannot span the window")
    {
        auto const huge = std::string("https://example.com/") + std::string(500, 'x');
        auto const entered = tracker.update(huge, cell(0, 0), 40);
        CHECK(entered.changed);
        CHECK(codepointCount(entered.text) == 40);
    }
}
