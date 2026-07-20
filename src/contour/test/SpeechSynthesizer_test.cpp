// SPDX-License-Identifier: Apache-2.0
//
// What gets spoken, and whether the feature offers itself at all. The synthesizer behind it is an
// optional Qt module and a platform voice, neither of which a test can rely on — so the part that is
// pinned here is the part that is ours: the text preparation, and the decision to stay quiet.

#include <contour/ContextMenu.h>
#include <contour/SpeechSynthesizer.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using contour::NullSpeechSynthesizer;
using contour::speakableText;

TEST_CASE("speakableText drops what only exists for the eye", "[contour][speech]")
{
    auto constexpr Max = size_t { 4000 };

    SECTION("grid padding is silence")
    {
        // A terminal line exists out to the right margin whether or not anything was written there, so
        // a selection carries the blanks along. Spoken verbatim they are a long pause per line.
        CHECK(speakableText("hello          \nworld     ", Max) == "hello\nworld");
        CHECK(speakableText("tabbed\t\t\n", Max) == "tabbed");
        // CRLF from a pasted or captured buffer must not leave a stray carriage return behind.
        CHECK(speakableText("one   \r\ntwo\r\n", Max) == "one\ntwo");
    }

    SECTION("a run of blank lines becomes one pause")
    {
        CHECK(speakableText("a\n\n\n\n\nb", Max) == "a\n\nb");
        // ...and padding-only lines count as blank, which is what makes the collapse work on real
        // terminal output rather than only on hand-written strings.
        CHECK(speakableText("a\n   \n      \nb", Max) == "a\n\nb");
    }

    SECTION("leading and trailing blank lines are pure padding")
    {
        CHECK(speakableText("\n\n  \nhello\n\n\n", Max) == "hello");
    }

    SECTION("nothing worth saying comes back empty")
    {
        CHECK(speakableText("", Max).empty());
        CHECK(speakableText("\n\n\n", Max).empty());
        CHECK(speakableText("      \n   ", Max).empty());
    }
}

TEST_CASE("speakableText bounds how much it will read", "[contour][speech]")
{
    // Selecting a build log must not become minutes of speech with no way to skip ahead.
    auto text = std::string {};
    for (auto i = 0; i < 500; ++i)
        text += "line " + std::to_string(i) + "\n";

    auto const spoken = speakableText(text, 100);
    CHECK(spoken.size() <= 100);
    CHECK_FALSE(spoken.empty());
    // Cut at a line boundary, so speech ends on something that sounds finished rather than mid-word.
    CHECK(spoken.back() != ' ');
    CHECK(spoken.find("line 0") == 0);

    SECTION("a single enormous line is still bounded")
    {
        // No newline to cut at: the bound must still hold rather than deferring to a boundary that is
        // not there.
        CHECK(speakableText(std::string(5000, 'x'), 100).size() == 100);
    }
}

TEST_CASE("without a speech engine the feature does not offer itself", "[contour][speech]")
{
    auto const nullSpeech = NullSpeechSynthesizer {};
    CHECK_FALSE(nullSpeech.available());

    // ...and the menu row goes with it. A row that is permanently dead teaches the user the feature is
    // broken, when in truth this build or this machine simply has no voice.
    auto state = contour::ContextMenuState {};
    state.hasSelection = true;
    state.canSpeak = false;
    auto const withoutSpeech = contour::buildContextMenu(state);
    auto const hasReadAloud = [](auto const& entries) {
        return std::ranges::any_of(entries, [](auto const& e) { return e.title == "Read Aloud"; });
    };
    CHECK_FALSE(hasReadAloud(withoutSpeech));

    state.canSpeak = true;
    CHECK(hasReadAloud(contour::buildContextMenu(state)));

    // With a voice but nothing selected there is nothing to read, so the row stays away.
    state.hasSelection = false;
    CHECK_FALSE(hasReadAloud(contour::buildContextMenu(state)));
}
