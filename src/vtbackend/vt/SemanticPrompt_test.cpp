// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SemanticPrompt.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>

using namespace vtbackend;
using namespace std::string_literals;
using namespace std::string_view_literals;

TEST_CASE("SemanticPrompt.promptKindOf reads the k= parameter", "[osc133]")
{
    // The parameters as Screen hands them over: everything after the mark's letter, leading ';' included.
    CHECK(promptKindOf("") == PromptKind::Initial);
    CHECK(promptKindOf(";click_events=1") == PromptKind::Initial);
    CHECK(promptKindOf(";k=i;click_events=1") == PromptKind::Initial);
    CHECK(promptKindOf(";k=s;click_events=1") == PromptKind::Continuation);
    CHECK(promptKindOf(";k=c") == PromptKind::Continuation);
    CHECK(promptKindOf(";aid=7;k=r") == PromptKind::Right);
}

TEST_CASE("SemanticPrompt.promptKindOf reads an unknown or empty kind as the prompt itself", "[osc133]")
{
    CHECK(promptKindOf(";k=zz") == PromptKind::Initial);
    CHECK(promptKindOf(";k") == PromptKind::Initial);
    CHECK(promptKindOf(";k=") == PromptKind::Initial);
}

TEST_CASE("SemanticPrompt.promptRedrawOf reads kitty's redraw=0", "[osc133]")
{
    // kitty's parse_prompt_mark: the token redraw=0 says the shell does not redraw its prompt; nothing else
    // does.
    CHECK(promptRedrawOf("") == PromptRedraw::OnResize);
    CHECK(promptRedrawOf(";redraw=0") == PromptRedraw::Never);
    CHECK(promptRedrawOf(";aid=7;redraw=0;click_events=1") == PromptRedraw::Never);
    CHECK(promptRedrawOf(";k=s;redraw=0") == PromptRedraw::Never);
    CHECK(promptRedrawOf(";redraw=1") == PromptRedraw::OnResize);
    CHECK(promptRedrawOf(";redraw=") == PromptRedraw::OnResize);
    CHECK(promptRedrawOf(";redraw") == PromptRedraw::OnResize);
    CHECK(promptRedrawOf(";redraw=00") == PromptRedraw::OnResize);
}

TEST_CASE("SemanticPrompt.firstShellWord reads kitty's shlex examples", "[osc133]")
{
    // kitty_tests/datatypes.py, test_shlex_split: each text and the first word kitty reads from it.
    auto const cases = {
        std::pair { R"(a"")"sv, "a"s },
        std::pair { R"(a""b)"sv, "ab"s },
        std::pair { R"(-1 "" 2)"sv, "-1"s },
        std::pair { R"("")"sv, ""s },
        std::pair { R"("ab")"sv, "ab"s },
        std::pair { R"(x "ab"y \m)"sv, "x"s },
        std::pair { R"(x'y"\z'1)"sv, R"(xy"\z1)"s },
        std::pair { R"(\abc\ d)"sv, "abc d"s },
        std::pair { ""sv, ""s },
        std::pair { "   "sv, ""s },
        std::pair { " \tabc\n\t\r "sv, "abc"s },
        std::pair { "\xF0\x9F\x98\x80 a"sv, "\xF0\x9F\x98\x80"s },
        std::pair { "\"a\xF0\x9F\x98\x80\""sv, "a\xF0\x9F\x98\x80"s },
        // ... and with ANSI-C quoting, as decode_cmdline reads it.
        std::pair { R"($'ab')"sv, "ab"s },
        std::pair { R"(1$'ab')"sv, "1ab"s },
        std::pair { R"("1$'ab'")"sv, R"(1$'ab')"s },
        std::pair { R"($'a\123b')"sv, "aSb"s },
        std::pair { R"($'a\1b')"sv, "a\x01"s + "b" },
        std::pair { R"($'a\12b')"sv, "a\nb"s },
        std::pair { R"($'a\db')"sv, "adb"s },
        std::pair { R"($'a\x1bb')"sv, "a\x1b"s + "b" },
        std::pair { R"($'\u123z')"sv, "\xC4\xA3z"s },
        std::pair { R"($'\U0001F1E8')"sv, "\xF0\x9F\x87\xA8"s },
        std::pair { R"($'\U1F1E8')"sv, "\xF0\x9F\x87\xA8"s },
        std::pair { R"($'a\U1F1E8'b)"sv, "a\xF0\x9F\x87\xA8"s + "b" },
        std::pair { R"($'a''b')"sv, "ab"s },
        std::pair { R"(x$''''''''AAAA)"sv, "xAAAA"s },
        std::pair { R"($''''''''AAAA)"sv, "AAAA"s },
        std::pair { R"($""'x')"sv, "$x"s },
        std::pair { R"(\$'x')"sv, "$x"s },
        std::pair { R"($\''x')"sv, "$'x"s },
    };
    for (auto const& [text, word]: cases)
    {
        INFO(text);
        auto const read = firstShellWord(text);
        REQUIRE(read.has_value());
        CHECK(*read == word);
    }
}

TEST_CASE("SemanticPrompt.firstShellWord refuses what kitty refuses", "[osc133]")
{
    auto const cases = {
        std::pair { R"(abc\)"sv, ShellQuotingError::TrailingBackslash },
        std::pair { R"(\)"sv, ShellQuotingError::TrailingBackslash },
        std::pair { R"("a\)"sv, ShellQuotingError::TrailingBackslash },
        std::pair { R"('abc)"sv, ShellQuotingError::UnterminatedQuote },
        std::pair { R"(')"sv, ShellQuotingError::UnterminatedQuote },
        std::pair { R"(")"sv, ShellQuotingError::UnterminatedQuote },
        std::pair { R"("a\")"sv, ShellQuotingError::UnterminatedQuote },
        std::pair { R"($''')"sv, ShellQuotingError::UnterminatedQuote },
        std::pair { R"(a$'''b)"sv, ShellQuotingError::UnterminatedQuote },
        std::pair { R"($'\x')"sv, ShellQuotingError::IncompleteEscape },
        std::pair { R"($'\c)"sv, ShellQuotingError::IncompleteEscape },
        std::pair { R"($'\uD800')"sv, ShellQuotingError::NotACodepoint },
    };
    for (auto const& [text, error]: cases)
    {
        INFO(text);
        auto const read = firstShellWord(text);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == error);
    }
}

TEST_CASE("SemanticPrompt.firstShellWord drops a code point past Unicode", "[osc133]")
{
    auto const read = firstShellWord(R"($'a\U110000z')");
    REQUIRE(read.has_value());
    CHECK(*read == "az");
}

TEST_CASE("SemanticPrompt.reportedCommandLine reads kitty's cmdline=", "[osc133]")
{
    // What kitty's shell integration tests type, as bash's `printf %q` quotes it.
    CHECK(reportedCommandLine(R"(;cmdline=mkdir\ test\ \&\&\ ls\ -a)") == "mkdir test && ls -a");
    CHECK(reportedCommandLine(R"(;cmdline=$'printf "%s\026\a%s" "a" "b"')")
          == "printf \"%s\x16\a%s\" \"a\" \"b\"");
    CHECK(reportedCommandLine(R"(;cmdline=cd\ /home/u/testing-cwd-notification-)"
                              "\xF0\x9F\x90\xB1")
          == "cd /home/u/testing-cwd-notification-\xF0\x9F\x90\xB1");

    // A ';' in the value is the command's own: it runs to the end of the sequence.
    CHECK(reportedCommandLine(R"(;cmdline=echo\ a\;\ b)") == "echo a; b");
    CHECK(reportedCommandLine(R"(;cmdline='echo a; b')") == "echo a; b");

    // Only the first word, as kitty reads it.
    CHECK(reportedCommandLine(";cmdline=ls -la") == "ls");
}

TEST_CASE("SemanticPrompt.reportedCommandLine reports nothing empty or malformed", "[osc133]")
{
    CHECK(reportedCommandLine("") == std::nullopt);
    CHECK(reportedCommandLine(";aid=7") == std::nullopt);
    CHECK(reportedCommandLine(";cmdline=") == std::nullopt);
    CHECK(reportedCommandLine(";cmdline=''") == std::nullopt);
    CHECK(reportedCommandLine(";cmdline=   ") == std::nullopt);
    CHECK(reportedCommandLine(";cmdline='ls") == std::nullopt);
    CHECK(reportedCommandLine(";cmdline_url=") == std::nullopt);
}

TEST_CASE("SemanticPrompt.reportedCommandLine reads cmdline_url= to the end of the sequence", "[osc133]")
{
    CHECK(reportedCommandLine(";cmdline_url=ls%20-la%20%2Ftmp") == "ls -la /tmp");
    CHECK(reportedCommandLine(";aid=7;cmdline_url=ls") == "ls");

    // As kitty reads it (decode_cmdline): a ';' in the value is the command's own, percent-encoded or not.
    CHECK(reportedCommandLine(";cmdline_url=echo%20a;%20b") == "echo a; b");
    CHECK(reportedCommandLine(";cmdline_url=echo%20a%3B%20b") == "echo a; b");
    CHECK(reportedCommandLine(";cmdline_url=ls;cmdline=pwd") == "ls;cmdline=pwd");
    CHECK(reportedCommandLine(";cmdline=ls;aid=7") == "ls;aid=7");
}
