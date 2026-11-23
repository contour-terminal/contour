// SPDX-License-Identifier: Apache-2.0
//
// The Windows spawn rules: which file a program starts, which PATH entries are searched, and how the
// environment's names compare. Pure, so tested on every platform.

#include <vtpty/WindowsProcessRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("windowsImageName appends .exe to a bare name and keeps an .exe", "[windowsprocessrules]")
{
    CHECK(vtpty::windowsImageName("git") == "git.exe");
    CHECK(vtpty::windowsImageName("git.exe") == "git.exe");
    CHECK(vtpty::windowsImageName("GIT.EXE") == "GIT.EXE");
    CHECK(vtpty::windowsImageName(R"(C:\Program Files\Git\cmd\git)")
          == R"(C:\Program Files\Git\cmd\git.exe)");
    CHECK(vtpty::windowsImageName(R"(C:\tools.d\x)") == R"(C:\tools.d\x.exe)"); // the dot is the directory's
}

TEST_CASE("windowsImageName refuses every other extension", "[windowsprocessrules]")
{
    for (auto const name: { "claude.cmd",
                            "x.bat",
                            "X.CMD",
                            "x.com",
                            "x.ps1",
                            "x.vbs",
                            "x.lnk",
                            "x.",
                            "",
                            R"(C:\a\b.cmd)",
                            "x.exe.cmd" })
    {
        CAPTURE(name);
        CHECK_FALSE(vtpty::windowsImageName(name).has_value());
    }
}

TEST_CASE("isAbsoluteWindowsPath", "[windowsprocessrules]")
{
    for (auto const path: { R"(C:\x)", "c:/x", R"(C:\)", R"(\\srv\share\x)", R"(\\?\C:\x)" })
        CHECK(vtpty::isAbsoluteWindowsPath(path));
    for (auto const path: { "C:x", R"(\x)", "x", R"(.\x)", ".", "", R"("C:\x")", "/usr/bin" })
    {
        CAPTURE(path);
        CHECK_FALSE(vtpty::isAbsoluteWindowsPath(path));
    }
}

TEST_CASE("findWindowsHeldDirectoryProgram searches only absolute PATH entries", "[windowsprocessrules]")
{
    auto const files =
        std::set<std::string> { R"(C:\bin\tool.exe)", R"(C:\other\tool.exe)", R"(bin\tool.exe)",
                                R"(.\tool.exe)",      R"(tool.exe)",          R"(C:\cmds\tool.cmd)" };
    auto const exists = [&](std::string const& candidate) {
        return files.contains(candidate);
    };
    using vtpty::findWindowsHeldDirectoryProgram;
    CHECK(findWindowsHeldDirectoryProgram("tool", R"(C:\bin;C:\other)", exists) == R"(C:\bin\tool.exe)");
    CHECK(findWindowsHeldDirectoryProgram("tool", R"(C:\bin\;C:\other)", exists) == R"(C:\bin\tool.exe)");
    for (auto const path: { R"(;C:\other)",
                            R"(.;C:\other)",
                            R"(bin;C:\other)",
                            R"(C:;C:\other)",
                            R"("C:\bin";C:\other)",
                            R"(\bin;C:\other)" })
    {
        CAPTURE(path);
        CHECK(findWindowsHeldDirectoryProgram("tool", path, exists) == R"(C:\other\tool.exe)");
    }
    CHECK_FALSE(findWindowsHeldDirectoryProgram("tool", R"(;.;bin)", exists).has_value());
    CHECK_FALSE(findWindowsHeldDirectoryProgram("tool", "", exists).has_value());
    CHECK_FALSE(findWindowsHeldDirectoryProgram("tool.cmd", R"(C:\cmds)", exists).has_value());
}

TEST_CASE("findWindowsHeldDirectoryProgram refuses a relative path with a directory", "[windowsprocessrules]")
{
    auto const always = [](std::string const&) {
        return true;
    };
    for (auto const program: { R"(.\tool)", R"(bin\tool.exe)", "bin/tool", "C:tool", R"(\tool)" })
    {
        CAPTURE(program);
        CHECK_FALSE(vtpty::findWindowsHeldDirectoryProgram(program, R"(C:\bin)", always).has_value());
    }
}

TEST_CASE("findWindowsHeldDirectoryProgram takes an absolute .exe as it is", "[windowsprocessrules]")
{
    auto const exists = [](std::string const& candidate) {
        return candidate == R"(C:\c\contour.exe)";
    };
    CHECK(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\contour.exe)", "", exists) == R"(C:\c\contour.exe)");
    CHECK(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\contour)", "", exists) == R"(C:\c\contour.exe)");
    CHECK_FALSE(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\run.bat)", "", exists).has_value());
    CHECK_FALSE(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\missing.exe)", "", exists).has_value());
}

TEST_CASE("windowsEnvironmentEntries collapses names that differ in case, the last winning",
          "[windowsprocessrules]")
{
    auto const environment = std::vector<std::pair<std::string, std::string>> {
        { "Path", R"(C:\a)" }, { "HOME", "h" }, { "PATH", R"(C:\b)" }, { "x", "1" }, { "X", "2" }
    };
    auto entries = vtpty::windowsEnvironmentEntries(environment);
    std::ranges::sort(entries);
    CHECK(entries == std::vector<std::string> { "HOME=h", R"(PATH=C:\b)", "X=2" });
}

TEST_CASE("windowsSearchPathOf finds PATH in any case", "[windowsprocessrules]")
{
    using Environment = std::vector<std::pair<std::string, std::string>>;
    CHECK(vtpty::windowsSearchPathOf(Environment { { "Path", R"(C:\a)" } }) == R"(C:\a)");
    CHECK(vtpty::windowsSearchPathOf(Environment { { "Path", R"(C:\a)" }, { "PATH", R"(C:\b)" } })
          == R"(C:\b)");
    CHECK(vtpty::windowsSearchPathOf(Environment { { "PATHEXT", ".EXE" } }).empty());
}
