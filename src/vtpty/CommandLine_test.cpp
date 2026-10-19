// SPDX-License-Identifier: Apache-2.0
//
// How a configured program written as a whole command line is read: a `shell:` value "wsl.exe -d Ubuntu"
// ran on Windows by accident while the program went to CreateProcess unquoted, and "bash -l" never ran
// on POSIX at all. ConPty_test.cpp and Process_test.cpp spawn real programs with the same rule.

#include <vtpty/CommandLine.hpp>
#include <vtpty/ProgramLookup.hpp>

#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <paths.h>
#endif

using vtpty::ProgramInvocation;
using vtpty::readAsCommandLine;

namespace
{

#ifdef _WIN32
/// What a program file's name ends in.
constexpr auto Executable = std::string_view { ".exe" };
#else
/// What a program file's name ends in.
constexpr auto Executable = std::string_view {};
#endif

/// @return A predicate that holds for exactly @p programs.
[[nodiscard]] vtpty::ProgramPredicate onlyThesePrograms(std::vector<std::string_view> programs)
{
    return [programs = std::move(programs)](std::string_view candidate) {
        return std::ranges::contains(programs, candidate);
    };
}

/// @return A filesystem holding exactly @p files, each an executable one.
[[nodiscard]] core::platform::testing::InMemoryFileSystem onlyTheseFiles(
    std::initializer_list<std::string> files)
{
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    for (auto const& file: files)
        fileSystem.addExecutable(std::filesystem::path { file });
    return fileSystem;
}

/// @return Where to look for a program among @p files: on @p path, and in @p workingDirectory for a relative
///         path.
[[nodiscard]] vtpty::ProgramSearch searchIn(core::platform::FileSystem const& files,
                                            std::string_view path = {},
                                            std::filesystem::path workingDirectory = {})
{
    return vtpty::ProgramSearch { .path = path,
                                  .workingDirectory = std::move(workingDirectory),
                                  .fileSystem = files };
}

/// @return @p entries joined into a PATH value.
[[nodiscard]] std::string pathList(std::vector<std::string> const& entries)
{
    return core::joinHumanReadable(entries, std::string { vtpty::PathListSeparator });
}

} // namespace

TEST_CASE("shellSplit tokenizes a command line respecting quotes", "[commandline]")
{
    using vtpty::shellSplit;
    using V = std::vector<std::string>;
    CHECK(shellSplit("emacs -nw") == V { "emacs", "-nw" });
    CHECK(shellSplit("  git   log --oneline ") == V { "git", "log", "--oneline" });
    CHECK(shellSplit("nvim") == V { "nvim" });
    CHECK(shellSplit("").empty());
    CHECK(shellSplit("   ").empty());
    // Double quotes group spaces into one token; the quotes themselves are stripped.
    CHECK(shellSplit(R"("/opt/my app/emacs" -nw)") == V { "/opt/my app/emacs", "-nw" });
    // Single quotes are literal (no inner processing).
    CHECK(shellSplit(R"(sh -c 'echo hi there')") == V { "sh", "-c", "echo hi there" });
    // A backslash escapes the next character outside quotes.
    CHECK(shellSplit(R"(a\ b c)") == V { "a b", "c" });
}

TEST_CASE("readAsCommandLine reads a program that names none as a command line", "[commandline]")
{
    // What follows the program first, then what `arguments:` adds: on Windows as written, for the child
    // to split itself; elsewhere as the words a POSIX shell reads.
    auto const configured = std::vector<std::string> { "--configured" };
    auto const reading = readAsCommandLine("wsl.exe -d Ubuntu", configured, onlyThesePrograms({ "wsl.exe" }));
    REQUIRE(reading.has_value());
#ifdef _WIN32
    CHECK(*reading
          == ProgramInvocation {
              .program = "wsl.exe", .writtenArguments = " -d Ubuntu", .arguments = { "--configured" } });
#else
    CHECK(*reading
          == ProgramInvocation { .program = "wsl.exe",
                                 .writtenArguments = {},
                                 .arguments = { "-d", "Ubuntu", "--configured" } });
#endif

    // "bash -l" names its program alike on every platform: it is looked up on PATH as any other.
    CHECK(readAsCommandLine("bash -l", {}, onlyThesePrograms({}))->program == "bash");

    // Quotes group a program path with a space in it, which then runs as one.
    CHECK(readAsCommandLine(R"("/opt/my app/sh" -l)", {}, onlyThesePrograms({}))->program
          == "/opt/my app/sh");
}

TEST_CASE("readAsCommandLine leaves a program that names one as it is", "[commandline]")
{
    // A path with spaces that names a program is never split: "C:\Program" is not what was meant.
    CHECK_FALSE(readAsCommandLine(R"(C:\Program Files\PowerShell\7\pwsh.exe)",
                                  {},
                                  onlyThesePrograms({ R"(C:\Program Files\PowerShell\7\pwsh.exe)" }))
                    .has_value());
    CHECK_FALSE(readAsCommandLine("/opt/my app/sh", {}, onlyThesePrograms({ "/opt/my app/sh" })).has_value());

    // A single word names a program or nothing, and no reading makes it either: the spawn falls back.
    CHECK_FALSE(readAsCommandLine("no-such-program", {}, onlyThesePrograms({})).has_value());
    CHECK_FALSE(readAsCommandLine("", {}, onlyThesePrograms({})).has_value());

    // Whitespace and nothing else has no words to read.
    CHECK_FALSE(readAsCommandLine("  \t ", {}, onlyThesePrograms({})).has_value());
}

TEST_CASE("readAsCommandLine reads a command line as this platform does", "[commandline]")
{
#ifdef _WIN32
    // A backslash separates directories; it escapes nothing.
    CHECK(readAsCommandLine(R"(C:\Windows\System32\wsl.exe -d Ubuntu)", {}, onlyThesePrograms({}))
          == ProgramInvocation { .program = R"(C:\Windows\System32\wsl.exe)",
                                 .writtenArguments = " -d Ubuntu",
                                 .arguments = {} });

    // An unquoted path with a space in it, as Git Bash is configured: what CreateProcess would run.
    CHECK(readAsCommandLine(R"(C:\Program Files\Git\bin\bash.exe --login)",
                            {},
                            onlyThesePrograms({ R"(C:\Program Files\Git\bin\bash.exe)" }))
          == ProgramInvocation { .program = R"(C:\Program Files\Git\bin\bash.exe)",
                                 .writtenArguments = " --login",
                                 .arguments = {} });

    // The rest as written, which cmd.exe reads by rules of its own: the C runtime's would split it at
    // "C:\Tools x", and quoting those words again would hand cmd.exe a different line.
    CHECK(readAsCommandLine(
              R"(cmd.exe /s /c ""C:\Tools x\app.exe" --flag")", {}, onlyThesePrograms({ "cmd.exe" }))
          == ProgramInvocation { .program = "cmd.exe",
                                 .writtenArguments = R"( /s /c ""C:\Tools x\app.exe" --flag")",
                                 .arguments = {} });
#else
    // As a POSIX shell reads it: single quotes are literal, a backslash escapes.
    CHECK(readAsCommandLine(R"(sh -c 'echo a\ b')", {}, onlyThesePrograms({}))
          == ProgramInvocation {
              .program = "sh", .writtenArguments = {}, .arguments = { "-c", R"(echo a\ b)" } });
#endif
}

TEST_CASE("buildWindowsCommandLine hands on the written arguments as they are", "[commandline]")
{
    // A command line configured as the program reaches CreateProcess as it was written, but for the
    // program, quoted where it holds whitespace -- with the configured arguments after it.
    auto const invocation = ProgramInvocation { .program = R"(C:\Windows\System32\cmd.exe)",
                                                .writtenArguments = R"( /s /c ""C:\Tools x\app.exe" --flag")",
                                                .arguments = {} };
    CHECK(vtpty::buildWindowsCommandLine(invocation)
          == R"(C:\Windows\System32\cmd.exe /s /c ""C:\Tools x\app.exe" --flag")");

    auto withArguments = invocation;
    withArguments.program = R"(C:\Program Files\x.exe)";
    withArguments.arguments = { "two words", "plain" };
    CHECK(vtpty::buildWindowsCommandLine(withArguments)
          == R"("C:\Program Files\x.exe" /s /c ""C:\Tools x\app.exe" --flag" "two words" plain)");

    // A program configured on its own has nothing written after it.
    CHECK(vtpty::buildWindowsCommandLine(
              ProgramInvocation { .program = "pwsh.exe", .writtenArguments = {}, .arguments = { "-NoLogo" } })
          == "pwsh.exe -NoLogo");
}

TEST_CASE("namesProgram looks a path up as it is and a name on PATH", "[commandline]")
{
    auto const search = std::format("/opt/empty{}/usr/bin", vtpty::PathListSeparator);
    auto const files = onlyTheseFiles(
        { std::format("/usr/bin/bash{}", Executable), std::format("/opt/my app/sh{}", Executable) });

    CHECK(vtpty::namesProgram("bash", searchIn(files, search)));
    CHECK(vtpty::namesProgram("/opt/my app/sh", searchIn(files, search)));
    CHECK_FALSE(vtpty::namesProgram("bash -l", searchIn(files, search)));
    CHECK_FALSE(vtpty::namesProgram("/opt/my app/sh -l", searchIn(files, search)));
    CHECK_FALSE(vtpty::namesProgram("zsh", searchIn(files, search)));
    CHECK_FALSE(vtpty::namesProgram("", searchIn(files, search)));

#ifdef _WIN32
    // CreateProcess appends ".exe" to a name without an extension, and so does the lookup.
    auto const windowsFiles = onlyTheseFiles({ "C:/Program Files/PowerShell/7/pwsh.exe" });
    CHECK(vtpty::namesProgram(R"(C:\Program Files\PowerShell\7\pwsh)", searchIn(windowsFiles)));
    CHECK(vtpty::namesProgram(R"(C:\Program Files\PowerShell\7\pwsh.exe)", searchIn(windowsFiles)));

    // And it never runs a file without an extension, so neither is one a program: a stray "C:\Program"
    // must not be read as the program "C:\Program Files\Git\bin\bash.exe --login" runs.
    auto const strayFiles = onlyTheseFiles({ "C:/Program", "C:/Program Files/Git/bin/bash.exe" });
    CHECK_FALSE(vtpty::namesProgram(R"(C:\Program)", searchIn(strayFiles)));
    CHECK(readAsCommandLine(
              R"(C:\Program Files\Git\bin\bash.exe --login)", {}, vtpty::programsIn(searchIn(strayFiles)))
              ->program
          == R"(C:\Program Files\Git\bin\bash.exe)");
#endif
}

#ifdef _WIN32
TEST_CASE("namesProgram counts an App Execution Alias as a program", "[commandline]")
{
    // A Store-installed pwsh.exe, winget or python is a zero-byte reparse point in WindowsApps, a
    // directory on PATH: CreateProcess runs it, but std::filesystem cannot open it, and so cannot tell
    // it is a file. The filesystem's own answer counts it.
    auto const localAppData =
        core::pathFromUtf8(core::LiveEnvironment {}.get("LOCALAPPDATA").value_or(std::string {}));
    if (!localAppData || localAppData->empty())
        SKIP("LOCALAPPDATA is not set");
    auto const aliases = *localAppData / "Microsoft" / "WindowsApps";

    auto alias = std::optional<std::filesystem::path> {};
    auto error = std::error_code {};
    for (auto const& entry: std::filesystem::directory_iterator { aliases, error })
    {
        if (entry.path().extension() == ".exe" && !std::filesystem::is_regular_file(entry.path(), error))
        {
            alias = entry.path();
            break;
        }
    }
    if (!alias)
        SKIP("no App Execution Alias on this machine");

    INFO("alias: " << core::pathToUtf8(*alias));
    auto const path = core::pathToUtf8(aliases);
    CHECK(vtpty::namesProgram(core::pathToUtf8(alias->stem()),
                              searchIn(core::platform::NativeFileSystem::instance(), path)));
}
#endif

TEST_CASE("namesProgram reads a relative path in the directory the program runs in", "[commandline]")
{
    // The child changes to its working directory before it execs, so that is where "./bin/my shell" is:
    // asked of this process's own, it named nothing there, and the program was split at its space.
    auto files = onlyTheseFiles({ std::format("/home/user/project/bin/my shell{}", Executable) });
    files.setCurrentPath("/");
    auto const search = searchIn(files, "", "/home/user/project");

    CHECK(vtpty::namesProgram("./bin/my shell", search));
    CHECK(vtpty::namesProgram("bin/my shell", search));
    CHECK_FALSE(readAsCommandLine("./bin/my shell", {}, vtpty::programsIn(search)).has_value());

    // An absolute path is where it says, whatever the directory.
    CHECK(vtpty::namesProgram("/home/user/project/bin/my shell", searchIn(files, "", "/elsewhere")));
}

TEST_CASE("programsFor looks a program up nowhere for the sandbox's host", "[commandline]")
{
    // flatpak-spawn runs it on the host, whose files and PATH the sandbox cannot see: a program the
    // sandbox happens to hold is no answer about the host. A command line holding whitespace is read as
    // the command line it spells.
    auto const files = onlyTheseFiles({ std::format("/usr/bin/npm run dev{}", Executable),
                                        std::format("/opt/my tool/bin/tool{}", Executable) });
    auto const search = searchIn(files, "/usr/bin");

    auto const here = vtpty::programsFor(vtpty::ProgramHost::ThisMachine, search);
    CHECK(here("npm run dev"));
    CHECK(here("/opt/my tool/bin/tool"));

    auto const host = vtpty::programsFor(vtpty::ProgramHost::SandboxHost, search);
    CHECK_FALSE(host("npm run dev"));
    CHECK_FALSE(host("/opt/my tool/bin/tool"));
    CHECK(vtpty::invocationToRun("npm run dev", {}, host).program == "npm");
}

TEST_CASE("resolveExecutablePath finds a bare name on PATH", "[commandline]")
{
    // The regression this pins: `execv` does NOT search PATH, and a client started as plain
    // `contour` hands exactly `contour` to the spawn as argv[0]. Exec'ing it verbatim fails with
    // ENOENT, so auto-spawn never worked for anyone who did not spell out a path.
    auto const search = pathList({ "/opt/empty", "/usr/local/bin", "/usr/bin" });
    auto const files = onlyTheseFiles({ "/usr/local/bin/contour", "/usr/bin/contour" });

    // Left-to-right: the FIRST matching entry wins, exactly as an exec would resolve it.
    CHECK(std::filesystem::path { vtpty::resolveExecutablePath("contour", search, files) }.generic_string()
          == "/usr/local/bin/contour");
}

TEST_CASE("resolveExecutablePath leaves a path-shaped name alone", "[commandline]")
{
    // execvp's own rule, and what keeps a relative or absolute argv[0] meaning what it says instead
    // of being silently redirected to some other install on PATH.
    auto const search = pathList({ "/usr/bin" });
    auto const files = onlyTheseFiles({ "/usr/bin/contour" });

    CHECK(vtpty::resolveExecutablePath("/opt/contour/contour", search, files) == "/opt/contour/contour");
    CHECK(vtpty::resolveExecutablePath("./contour", search, files) == "./contour");
}

TEST_CASE("resolveExecutablePath falls back to the name it was given", "[commandline]")
{
    auto const noFiles = onlyTheseFiles({});
    // Nothing matched: hand back the original so the exec still reports the name the user typed
    // (ENOENT on "contour" is a far better diagnostic than ENOENT on an invented path).
    CHECK(vtpty::resolveExecutablePath("contour", pathList({ "/usr/bin" }), noFiles) == "contour");
    // No PATH at all, and an empty name, are both just "nothing to search".
    CHECK(vtpty::resolveExecutablePath("contour", "", noFiles) == "contour");
    CHECK(vtpty::resolveExecutablePath("", pathList({ "/usr/bin" }), noFiles).empty());
}

TEST_CASE("execSearchPath is the environment's PATH, or the C library's default without one", "[commandline]")
{
    // execvp()'s choice, kept by a spawn that looks the program up before the fork and runs execv().
    CHECK(vtpty::execSearchPath("/opt/tools/bin:/usr/bin") == "/opt/tools/bin:/usr/bin");
    CHECK(vtpty::execSearchPath(std::string {}).empty()); // set, but empty: searched as it is
#ifndef _WIN32
    // Unset: the default the C library's own execvp() searches.
    auto const fallback = vtpty::execSearchPath(std::nullopt);
    CHECK_FALSE(fallback.ends_with('\0'));
    #ifdef __GLIBC__
    // glibc's CS_PATH, which its execvpe() searches: the distribution's choice ("/bin:/usr/bin" upstream,
    // "/usr/bin" on Fedora), so only that there is one is asserted.
    CHECK_FALSE(fallback.empty());
    #elifdef _PATH_DEFPATH
    CHECK(fallback == _PATH_DEFPATH); // FreeBSD's, Darwin's and musl's execvp() search it
    #else
    CHECK_FALSE(fallback.empty());
    #endif
#endif
}

TEST_CASE("resolveExecutablePath skips empty PATH entries", "[commandline]")
{
    // POSIX reads an empty entry as the current directory. Resolving argv[0] against the cwd is
    // exactly the ambiguity this function removes, so an empty entry must not match anything.
    auto fileSystem = onlyTheseFiles({ "/work/contour", "/usr/bin/contour" });
    fileSystem.setCurrentPath("/work");
    auto const search = pathList({ "", "/usr/bin" });
    CHECK(
        std::filesystem::path { vtpty::resolveExecutablePath("contour", search, fileSystem) }.generic_string()
        == "/usr/bin/contour");
}
