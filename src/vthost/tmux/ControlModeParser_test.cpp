// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <variant>

#include <vthost/tmux/ControlModeParser.h>

using namespace vthost::tmux;

TEST_CASE("unescapeOutput reverses tmux's octal escaping exactly", "[vthost][gateway]")
{
    CHECK(unescapeOutput("plain") == "plain");
    CHECK(unescapeOutput("\\033[1m") == "\033[1m");
    CHECK(unescapeOutput("a\\134b") == "a\\b");
    CHECK(unescapeOutput("\\015\\012") == "\r\n");
    // A backslash NOT followed by three octal digits passes through raw.
    CHECK(unescapeOutput("\\9x") == "\\9x");
    CHECK(unescapeOutput("\\01") == "\\01");
    // The largest byte tmux emits, \377 (0xFF), decodes exactly.
    CHECK(unescapeOutput("\\377") == "\xff");
    // An octal sequence that would decode ABOVE 0xFF (a leading digit of 4-7) is not one tmux emits;
    // it must pass through literally, NOT be truncated to a wrong byte (\400 would become 0x00).
    CHECK(unescapeOutput("\\400") == "\\400");
    CHECK(unescapeOutput("\\777") == "\\777");
    // UTF-8 passes through untouched (tmux never escapes >= 0x80).
    CHECK(unescapeOutput("caf\xc3\xa9") == "caf\xc3\xa9");
}

TEST_CASE("guards parse their triple and error flavor", "[vthost][gateway]")
{
    CHECK(classifyLine("%begin 1234 7 1")
          == ControlEvent { GuardBegin { .time = 1234, .number = 7, .flags = 1 } });
    CHECK(classifyLine("%end 1234 7 1")
          == ControlEvent { GuardEnd { .time = 1234, .number = 7, .flags = 1, .isError = false } });
    CHECK(classifyLine("%error 1234 7 1")
          == ControlEvent { GuardEnd { .time = 1234, .number = 7, .flags = 1, .isError = true } });
}

TEST_CASE("output notifications unescape and extended output splits the age", "[vthost][gateway]")
{
    CHECK(classifyLine("%output %3 hi\\015\\012")
          == ControlEvent { OutputEvent { .pane = 3, .bytes = "hi\r\n", .ageMs = std::nullopt } });
    // ONE age field before " : " (control.c:653-658).
    CHECK(classifyLine("%extended-output %3 250 : data")
          == ControlEvent { OutputEvent { .pane = 3, .bytes = "data", .ageMs = 250 } });
}

TEST_CASE("window and session notifications carry their ids", "[vthost][gateway]")
{
    CHECK(classifyLine("%window-add @5") == ControlEvent { WindowAddEvent { .window = 5 } });
    CHECK(classifyLine("%window-close @5") == ControlEvent { WindowCloseEvent { .window = 5 } });
    CHECK(classifyLine("%unlinked-window-close @5") == ControlEvent { WindowCloseEvent { .window = 5 } });
    CHECK(classifyLine("%window-renamed @5 build logs")
          == ControlEvent { WindowRenamedEvent { .window = 5, .name = "build logs" } });
    CHECK(classifyLine("%session-changed $0 main")
          == ControlEvent { SessionChangedEvent { .session = 0, .name = "main" } });
    CHECK(classifyLine("%layout-change @2 b25e,80x24,0,0,1 b25e,80x24,0,0,1 ")
          == ControlEvent { LayoutChangeEvent { .window = 2, .layout = "b25e,80x24,0,0,1" } });
    CHECK(classifyLine("%pause %4") == ControlEvent { PauseEvent { .pane = 4, .paused = true } });
    CHECK(classifyLine("%continue %4") == ControlEvent { PauseEvent { .pane = 4, .paused = false } });
    CHECK(classifyLine("%exit detached") == ControlEvent { ExitEvent { .reason = "detached" } });
}

TEST_CASE("non-notifications are body; unknown verbs are tolerated", "[vthost][gateway]")
{
    CHECK(classifyLine("0: @1 name [b25e,80x24,0,0,1]")
          == ControlEvent { BodyLine { .text = "0: @1 name [b25e,80x24,0,0,1]" } });
    CHECK(std::holds_alternative<UnknownNotification>(classifyLine("%subscription-changed x")));
    // The verb must end exactly: "%endless" is not "%end".
    CHECK(std::holds_alternative<UnknownNotification>(classifyLine("%endless 1 2 3")));
}
