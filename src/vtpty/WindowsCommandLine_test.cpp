// SPDX-License-Identifier: Apache-2.0
//
// How a program and its arguments are spelled into the one command line CreateProcess() takes. The
// child splits that line itself, by the Microsoft C runtime's rules ("Parsing C command-line arguments"
// on Microsoft Learn), so every row here is checked against what those rules make of it. ConPty_test.cpp
// runs a real child with the hardest of them.

#include <vtpty/WindowsCommandLine.hpp>
#include <vtpty/test/WindowsCommandLineSplit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

using vtpty::buildWindowsCommandLine;

namespace
{

/// @return A predicate that holds for exactly @p programs.
[[nodiscard]] vtpty::ProgramPredicate onlyThesePrograms(std::vector<std::string_view> programs)
{
    return [programs = std::move(programs)](std::string_view candidate) {
        return std::ranges::contains(programs, candidate);
    };
}

/// @return The command line for running "p" with @p argument alone.
[[nodiscard]] std::string withArgument(std::string_view argument)
{
    auto const arguments = std::array { std::string { argument } };
    return buildWindowsCommandLine("p", arguments);
}

/// Each argument, and the command line that runs "p" with it alone.
constexpr auto ArgumentCases = std::array {
    // As they are: nothing in them the C runtime reads specially.
    std::pair { "plain"sv, "p plain"sv },
    std::pair { R"(C:\dir\file)"sv, R"(p C:\dir\file)"sv },
    std::pair { R"(trailing\)"sv, R"(p trailing\)"sv },
    std::pair { "50%;'$`"sv, "p 50%;'$`"sv },
    // Whitespace: quoted.
    std::pair { "two words"sv, R"(p "two words")"sv },
    std::pair { "tab\there"sv, "p \"tab\there\""sv },
    std::pair { "line\nbreak"sv, "p \"line\nbreak\""sv },
    // Empty: quoted, or it would vanish.
    std::pair { ""sv, R"(p "")"sv },
    // A double quote: escaped, inside quotes.
    std::pair { R"(say "hi")"sv, R"(p "say \"hi\"")"sv },
    std::pair { R"(")"sv, R"(p "\"")"sv },
    // Backslashes before a double quote: doubled, plus the one escaping the quote.
    std::pair { R"(a\"b)"sv, R"(p "a\\\"b")"sv },
    std::pair { R"(a\\"b)"sv, R"(p "a\\\\\"b")"sv },
    // Backslashes before the closing quote: doubled, so that it still closes.
    std::pair { R"(C:\Program Files\)"sv, R"(p "C:\Program Files\\")"sv },
    std::pair { R"(two\\ \\)"sv, R"(p "two\\ \\\\")"sv },
    // Backslashes before anything else: as they are.
    std::pair { R"(a\\b c)"sv, R"(p "a\\b c")"sv },
};

} // namespace

TEST_CASE("buildWindowsCommandLine quotes a program path holding whitespace", "[windowscommandline]")
{
    // A program name is read up to the next double quote, without escapes: quoting is all it needs.
    CHECK(buildWindowsCommandLine(R"(C:\Program Files\PowerShell\7\pwsh.exe)", {})
          == R"("C:\Program Files\PowerShell\7\pwsh.exe")");
    CHECK(buildWindowsCommandLine("C:\\tab\there\\x.exe", {}) == "\"C:\\tab\there\\x.exe\"");
    CHECK(buildWindowsCommandLine(R"(C:\Windows\System32\cmd.exe)", {}) == R"(C:\Windows\System32\cmd.exe)");
    CHECK(buildWindowsCommandLine("pwsh.exe", {}) == "pwsh.exe");

    // Quoted already by whoever configured it; a file name cannot hold a double quote.
    CHECK(buildWindowsCommandLine(R"("C:\Program Files\x.exe")", {}) == R"("C:\Program Files\x.exe")");
}

TEST_CASE("buildWindowsCommandLine spells each argument so the C runtime reads it back",
          "[windowscommandline]")
{
    for (auto const& [argument, commandLine]: ArgumentCases)
    {
        INFO("argument: " << argument);
        CHECK(withArgument(argument) == commandLine);
    }
}

TEST_CASE("quoteWindowsArgument quotes an argument whether it needs it or not", "[windowscommandline]")
{
    CHECK(vtpty::quoteWindowsArgument("daemon") == R"("daemon")");
    CHECK(vtpty::quoteWindowsArgument(R"(--config=C:\dir\)") == R"("--config=C:\dir\\")");
    CHECK(vtpty::quoteWindowsArgument(R"(say "hi")") == R"("say \"hi\"")");
}

TEST_CASE("splitWindowsCommandLine reads a command line as the C runtime does", "[windowscommandline]")
{
    using vtpty::testing::splitWindowsCommandLine;
    using V = std::vector<std::string>;

    // Microsoft's own examples ("Parsing C command-line arguments", Microsoft Learn).
    CHECK(splitWindowsCommandLine(R"(p "abc" d e)") == V { "p", "abc", "d", "e" });
    CHECK(splitWindowsCommandLine(R"(p a\\b d"e f"g h)") == V { "p", R"(a\\b)", "de fg", "h" });
    CHECK(splitWindowsCommandLine(R"(p a\\\"b c d)") == V { "p", R"(a\"b)", "c", "d" });
    CHECK(splitWindowsCommandLine(R"(p a\\\\"b c" d e)") == V { "p", R"(a\\b c)", "d", "e" });
    CHECK(splitWindowsCommandLine(R"(p a"b"" c d)") == V { "p", R"(ab" c d)" });

    // The program: a double quote only groups, and a backslash separates directories.
    CHECK(splitWindowsCommandLine(R"(C:\Windows\System32\wsl.exe -d Ubuntu)")
          == V { R"(C:\Windows\System32\wsl.exe)", "-d", "Ubuntu" });
    CHECK(splitWindowsCommandLine(R"("C:\Program Files\x\"y.exe  -a)")
          == V { R"(C:\Program Files\x\y.exe)", "-a" });

    CHECK(splitWindowsCommandLine("").empty());
    CHECK(splitWindowsCommandLine(" \t ").empty());
}

TEST_CASE("splitWindowsCommandLine reads back what buildWindowsCommandLine spells", "[windowscommandline]")
{
    for (auto const& [argument, commandLine]: ArgumentCases)
    {
        INFO("argument: " << argument);
        CHECK(vtpty::testing::splitWindowsCommandLine(commandLine)
              == std::vector<std::string> { "p", std::string { argument } });
    }

    auto const program = std::string { R"(C:\Program Files\PowerShell\7\pwsh.exe)" };
    auto const arguments = std::vector<std::string> { "-NoLogo", "", R"(say "hi")", R"(C:\dir with space\)" };
    auto expected = std::vector<std::string> { program };
    expected.insert(expected.end(), arguments.begin(), arguments.end());
    CHECK(vtpty::testing::splitWindowsCommandLine(buildWindowsCommandLine(program, arguments)) == expected);
}

TEST_CASE("buildWindowsArguments spells the arguments alone", "[windowscommandline]")
{
    CHECK(vtpty::buildWindowsArguments({}).empty());
    auto const arguments = std::vector<std::string> { "", "plain", "two words" };
    CHECK(vtpty::buildWindowsArguments(arguments) == R"("" plain "two words")");
}

TEST_CASE("readWindowsProgram reads the program as CreateProcess does, and leaves the rest as it is",
          "[windowscommandline]")
{
    using vtpty::readWindowsProgram;
    using R = vtpty::WindowsProgramReading;
    auto const gitBash = R"(C:\Program Files\Git\bin\bash.exe)"sv;

    // The program runs up to the first whitespace at which it names one.
    CHECK(
        readWindowsProgram(R"(C:\Program Files\Git\bin\bash.exe --login -i)", onlyThesePrograms({ gitBash }))
        == R { .program = std::string { gitBash }, .rest = " --login -i" });

    // Tried from the left, so the shortest program wins, as it does for CreateProcess.
    CHECK(readWindowsProgram(R"(C:\Program Files\Git\bin\bash.exe --login)",
                             onlyThesePrograms({ gitBash, R"(C:\Program)" }))
          == R { .program = R"(C:\Program)", .rest = R"( Files\Git\bin\bash.exe --login)" });
    CHECK(readWindowsProgram(R"(C:\a b c d e)", onlyThesePrograms({ R"(C:\a b c)", R"(C:\a b c d)" }))
          == R { .program = R"(C:\a b c)", .rest = " d e" });

    // Blanks before the program are dropped; runs of them inside it end one word, not several.
    CHECK(readWindowsProgram("\t a b  \t c ", onlyThesePrograms({ "a b" }))
          == R { .program = "a b", .rest = "  \t c " });

    // The rest is as written, quotes and backslashes and all: the child reads it, not us.
    CHECK(readWindowsProgram(R"(C:\my app\x.exe "a b" c\\"d e")", onlyThesePrograms({ R"(C:\my app\x.exe)" }))
          == R { .program = R"(C:\my app\x.exe)", .rest = R"( "a b" c\\"d e")" });
    CHECK(
        readWindowsProgram(R"(cmd.exe /s /c ""C:\Tools x\app.exe" --flag")", onlyThesePrograms({ "cmd.exe" }))
        == R { .program = "cmd.exe", .rest = R"( /s /c ""C:\Tools x\app.exe" --flag")" });

    // Where no prefix names a program, the first word is the program, which then does not run.
    CHECK(readWindowsProgram(R"(C:\Program Files\x.exe -a)", onlyThesePrograms({}))
          == R { .program = R"(C:\Program)", .rest = R"( Files\x.exe -a)" });

    // A quoted program runs up to its closing quote, and nothing inside is tried.
    CHECK(readWindowsProgram(R"("C:\my app\x.exe" -a)", onlyThesePrograms({ R"("C:\my)" }))
          == R { .program = R"(C:\my app\x.exe)", .rest = " -a" });

    CHECK(readWindowsProgram("", onlyThesePrograms({})) == R {});
    CHECK(readWindowsProgram(" \t ", onlyThesePrograms({})) == R {});
}

TEST_CASE("buildWindowsCommandLine separates the arguments by one space each", "[windowscommandline]")
{
    auto const arguments = std::vector<std::string> { "-NoLogo", "", "-Command", ". 'x y.ps1'" };
    CHECK(buildWindowsCommandLine(R"(C:\Program Files\PowerShell\7\pwsh.exe)", arguments)
          == R"("C:\Program Files\PowerShell\7\pwsh.exe" -NoLogo "" -Command ". 'x y.ps1'")");
}
