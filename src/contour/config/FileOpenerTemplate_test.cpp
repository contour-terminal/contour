// SPDX-License-Identifier: Apache-2.0
#include <contour/config/FileOpenerTemplate.hpp>

#include <vtbackend/links/LinkTypes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using contour::config::FileOpenerError;
using contour::config::FileOpenerTemplate;
using Args = std::vector<std::string>;

TEST_CASE("FileOpenerTemplate.substitutesInsideEachArgument", "[config][links]")
{
    auto const opener = FileOpenerTemplate::parse("code --goto {path}:{line}:{column}");
    REQUIRE(opener);
    // A file name with a space or a `;` stays one argument: no shell reads it.
    CHECK(opener->expand("/w/a b;c.txt", vtbackend::LinkLocation { .line = 12, .column = 5 })
          == Args { "code", "--goto", "/w/a b;c.txt:12:5" });
    CHECK(opener->expand("/w/x", std::nullopt) == Args { "code", "--goto", "/w/x:1:1" });
}

TEST_CASE("FileOpenerTemplate.placeholdersInAnyArgument", "[config][links]")
{
    auto const opener = FileOpenerTemplate::parse("nvim +{line} {path}");
    REQUIRE(opener);
    CHECK(opener->expand("/w/x", vtbackend::LinkLocation { .line = 7, .column = 1 })
          == Args { "nvim", "+7", "/w/x" });
}

TEST_CASE("FileOpenerTemplate.aProgramAloneOpensThePathNowhere", "[config][links]")
{
    // No {path}: the program runs with what it was given, and nothing else is added.
    auto const opener = FileOpenerTemplate::parse("my-viewer");
    REQUIRE(opener);
    CHECK(opener->expand("/w/x", std::nullopt) == Args { "my-viewer" });
}

TEST_CASE("FileOpenerTemplate.rejectsWhatItCannotRun", "[config][links]")
{
    CHECK(FileOpenerTemplate::parse("").error() == FileOpenerError::Empty);
    CHECK(FileOpenerTemplate::parse("   ").error() == FileOpenerError::Empty);
    CHECK(FileOpenerTemplate::parse("code {file}").error() == FileOpenerError::UnknownPlaceholder);
    CHECK(FileOpenerTemplate::parse("code {}").error() == FileOpenerError::UnknownPlaceholder);
    CHECK(FileOpenerTemplate::parse("code {path").error() == FileOpenerError::UnterminatedPlaceholder);
    CHECK(FileOpenerTemplate::parse("code --goto {path}:{line").error()
          == FileOpenerError::UnterminatedPlaceholder);
    // The program is written out: neither empty, which a pane would replace with the shell, nor filled from
    // printed text, which a pane's spawn reads as a command line again.
    CHECK(FileOpenerTemplate::parse(R"("" {path})").error() == FileOpenerError::ProgramNotWrittenOut);
    CHECK(FileOpenerTemplate::parse("{path} --x").error() == FileOpenerError::ProgramNotWrittenOut);
    CHECK(FileOpenerTemplate::parse("/opt/{line}/edit {path}").error()
          == FileOpenerError::ProgramNotWrittenOut);
}

TEST_CASE("FileOpenerTemplate.aPrintedNameStaysOneArgumentHoweverItReads", "[config][links]")
{
    // Quotes, a command substitution and a leading dash are a file's name here, not words: no shell reads it.
    auto const opener = FileOpenerTemplate::parse("editor {path}");
    REQUIRE(opener);
    auto const name = std::string { R"(/w/-x $(id) "q" 'r'.txt)" };
    CHECK(opener->expand(name, std::nullopt) == Args { "editor", name });
}

TEST_CASE("FileOpenerTemplate.describesEveryError", "[config][links]")
{
    for (auto const error: { FileOpenerError::Empty,
                             FileOpenerError::UnknownPlaceholder,
                             FileOpenerError::UnterminatedPlaceholder,
                             FileOpenerError::ProgramNotWrittenOut })
    {
        auto const text = contour::config::describe(error);
        CHECK_FALSE(text.empty());
        CHECK_FALSE(text.ends_with('.'));
    }
    CHECK(contour::config::describe(FileOpenerError::UnknownPlaceholder).contains("{path}"));
}

#ifndef _WIN32
TEST_CASE("FileOpenerTemplate.readsTheCommandAsAConfiguredProgramIsRead", "[config][links]")
{
    // As a profile's `shell:` is read on POSIX: quotes group, so a program in a directory with a space runs.
    auto const opener = FileOpenerTemplate::parse("'/opt/My Editor/edit' --line={line} \"{path}\"");
    REQUIRE(opener);
    CHECK(opener->expand("/w/x y", vtbackend::LinkLocation { .line = 3, .column = 1 })
          == Args { "/opt/My Editor/edit", "--line=3", "/w/x y" });
}
#endif

TEST_CASE("FileOpenerTemplate.splitsWindowsArgumentsKeepingBackslashes", "[config][links]")
{
    CHECK(contour::config::detail::splitWrittenArguments(R"(--goto "C:\My Docs\{path}" -n)")
          == Args { "--goto", R"(C:\My Docs\{path})", "-n" });
    CHECK(contour::config::detail::splitWrittenArguments("  a\t\"\"  b ") == Args { "a", "", "b" });
    CHECK(contour::config::detail::splitWrittenArguments("").empty());
}
