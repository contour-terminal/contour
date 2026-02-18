// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/HintModeHandler.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ranges>

using namespace vtbackend;

namespace
{

/// Test executor that records callbacks.
class MockExecutor: public HintModeHandler::Executor
{
  public:
    std::string lastSelectedText;
    HintAction lastAction = HintAction::Copy;
    int hintSelectedCount = 0;
    int hintEnteredCount = 0;
    int hintExitedCount = 0;
    int redrawCount = 0;

    void onHintSelected(std::string const& matchedText, HintAction action) override
    {
        lastSelectedText = matchedText;
        lastAction = action;
        ++hintSelectedCount;
    }

    void onHintModeEntered() override { ++hintEnteredCount; }
    void onHintModeExited() override { ++hintExitedCount; }
    void requestRedraw() override { ++redrawCount; }
};

auto const allPatterns = HintModeHandler::builtinPatterns();

/// Returns only the URL pattern for precise count-based test assertions.
auto urlOnlyPatterns() -> std::vector<HintPattern>
{
    auto result = std::vector<HintPattern>();
    for (auto& p: HintModeHandler::builtinPatterns())
        if (p.name == "url")
            result.push_back(std::move(p));
    return result;
}

/// Returns only the IPv6 pattern for precise test assertions.
auto ipv6OnlyPatterns() -> std::vector<HintPattern>
{
    auto result = std::vector<HintPattern>();
    for (auto& p: HintModeHandler::builtinPatterns())
        if (p.name == "ipv6")
            result.push_back(std::move(p));
    return result;
}

/// Returns only the filepath pattern for precise test assertions.
auto filepathOnlyPatterns() -> std::vector<HintPattern>
{
    auto result = std::vector<HintPattern>();
    for (auto& p: HintModeHandler::builtinPatterns())
        if (p.name == "filepath")
            result.push_back(std::move(p));
    return result;
}

} // namespace

TEST_CASE("HintModeHandler.LabelAssignment.SingleChar", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> {
        "visit https://example.com for more",
        "also https://test.org and https://other.net",
    };

    handler.activate(lines, PageSize { LineCount(2), ColumnCount(50) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    auto const& matches = handler.matches();
    REQUIRE(matches.size() == 3);

    // Single-char labels for <=26 matches.
    CHECK(matches[0].label == "a");
    CHECK(matches[1].label == "b");
    CHECK(matches[2].label == "c");

    // Check matched text.
    CHECK(matches[0].matchedText == "https://example.com");
    CHECK(matches[1].matchedText == "https://test.org");
    CHECK(matches[2].matchedText == "https://other.net");

    // Check positions.
    CHECK(matches[0].start.line == LineOffset(0));
    CHECK(matches[0].start.column == ColumnOffset(6));
}

TEST_CASE("HintModeHandler.LabelAssignment.TwoChar", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Create 27 URLs to trigger two-char labels.
    auto lines = std::vector<std::string>();
    auto line = std::string {};
    for (auto const i: std::views::iota(0, 27))
    {
        line += std::format("https://site{}.com ", i);
        if (i % 5 == 4)
        {
            lines.push_back(line);
            line.clear();
        }
    }
    if (!line.empty())
        lines.push_back(line);

    handler.activate(lines,
                     PageSize { LineCount(static_cast<int>(lines.size())), ColumnCount(200) },
                     urlOnlyPatterns(),
                     HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 27);

    // Two-char labels.
    CHECK(handler.matches()[0].label == "aa");
    CHECK(handler.matches()[1].label == "ab");
    CHECK(handler.matches()[25].label == "az");
    CHECK(handler.matches()[26].label == "ba");
}

TEST_CASE("HintModeHandler.ProgressiveFiltering", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> {
        "https://alpha.com https://beta.com https://gamma.com",
    };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 3);
    CHECK(handler.matches()[0].label == "a");
    CHECK(handler.matches()[1].label == "b");
    CHECK(handler.matches()[2].label == "c");

    // Type 'b' — should filter to only match 'b' and auto-select.
    handler.processInput(U'b');

    CHECK(executor.hintSelectedCount == 1);
    CHECK(executor.lastSelectedText == "https://beta.com");
    CHECK(!handler.isActive()); // Should have deactivated after selection.
}

TEST_CASE("HintModeHandler.EscapeCancels", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "https://example.com" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());

    handler.processInput(U'\x1B');

    CHECK(!handler.isActive());
    CHECK(executor.hintExitedCount == 1);
    CHECK(executor.hintSelectedCount == 0); // No selection made.
}

TEST_CASE("HintModeHandler.NoMatches", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "no urls or hashes here" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    CHECK(handler.matches().empty());
}

TEST_CASE("HintModeHandler.FilePathPattern", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "edit /home/user/file.txt and ./local/path" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(50) }, allPatterns, HintAction::Open);

    REQUIRE(handler.isActive());
    // Should find file paths.
    auto foundHome = false;
    auto foundLocal = false;
    for (auto const& m: handler.matches())
    {
        if (m.matchedText.find("/home/user/file.txt") != std::string::npos)
            foundHome = true;
        if (m.matchedText.find("./local/path") != std::string::npos)
            foundLocal = true;
    }
    CHECK(foundHome);
    CHECK(foundLocal);
}

TEST_CASE("HintModeHandler.GitHashPattern", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "commit a1b2c3d some message" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(40) }, allPatterns, HintAction::Copy);

    REQUIRE(handler.isActive());
    auto foundHash = false;
    for (auto const& m: handler.matches())
    {
        if (m.matchedText == "a1b2c3d")
            foundHash = true;
    }
    CHECK(foundHash);
}

TEST_CASE("HintModeHandler.BackspaceRemovesFilter", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> {
        "https://alpha.com https://beta.com https://gamma.com",
    };

    auto const patterns = urlOnlyPatterns();

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, patterns, HintAction::Copy);

    REQUIRE(handler.matches().size() == 3);

    // Start typing but then backspace.
    handler.processInput(U'a');
    CHECK(!handler.isActive()); // 'a' is unique label -> auto-selected.

    // Reactivate for backspace test.
    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, patterns, HintAction::Copy);

    // Test backspace on empty filter is a no-op.
    handler.processInput(U'\x08'); // Backspace on empty filter.
    CHECK(handler.isActive());     // Should still be active.
    CHECK(handler.currentFilter().empty());
}

TEST_CASE("HintModeHandler.CaseInsensitive", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "https://example.com" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].label == "a");

    // Type uppercase 'A' — should be normalized to 'a'.
    handler.processInput(U'A');
    CHECK(executor.hintSelectedCount == 1);
    CHECK(executor.lastSelectedText == "https://example.com");
}

TEST_CASE("HintModeHandler.ActionDispatch", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "https://example.com" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, urlOnlyPatterns(), HintAction::Open);

    handler.processInput(U'a');

    CHECK(executor.hintSelectedCount == 1);
    CHECK(executor.lastAction == HintAction::Open);
}

TEST_CASE("HintModeHandler.OverlappingPatterns", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // URL "https://example.com/path" also matches filepath "/example.com/path".
    // The overlap removal should keep only the longer URL match.
    auto lines = std::vector<std::string> { "visit https://example.com/path for info" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(50) }, allPatterns, HintAction::Copy);

    REQUIRE(handler.isActive());

    // Check that no two matches overlap.
    auto const& matches = handler.matches();
    for (auto const i: std::views::iota(size_t { 1 }, matches.size()))
    {
        if (matches[i].start.line == matches[i - 1].start.line)
        {
            CHECK(matches[i].start.column > matches[i - 1].end.column);
        }
    }

    // The URL match should be present (it's the longer one).
    auto foundUrl = false;
    for (auto const& m: matches)
    {
        if (m.matchedText == "https://example.com/path")
            foundUrl = true;
    }
    CHECK(foundUrl);
}

TEST_CASE("HintModeHandler.BareRelativeFilePath", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Bare relative paths like those from git status or compiler output.
    auto lines = std::vector<std::string> {
        "modified: src/vtbackend/Terminal.cpp",
        "error in lib/utils/helpers.h:42",
    };

    handler.activate(
        lines, PageSize { LineCount(2), ColumnCount(50) }, filepathOnlyPatterns(), HintAction::Open);

    REQUIRE(handler.isActive());

    auto foundTerminal = false;
    auto foundHelpers = false;
    for (auto const& m: handler.matches())
    {
        if (m.matchedText == "src/vtbackend/Terminal.cpp")
            foundTerminal = true;
        if (m.matchedText.find("lib/utils/helpers.h") != std::string::npos)
            foundHelpers = true;
    }
    CHECK(foundTerminal);
    CHECK(foundHelpers);
}

TEST_CASE("HintModeHandler.BareRelativeDoesNotMatchPlainWords", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Plain words without slashes must NOT match the filepath pattern.
    auto lines = std::vector<std::string> { "hello world foo bar" };

    handler.activate(
        lines, PageSize { LineCount(1), ColumnCount(30) }, filepathOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    CHECK(handler.matches().empty());
}

TEST_CASE("HintModeHandler.ValidatorFiltersMatches", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "open /accept/path and /reject/path" };

    // Create a filepath pattern with a validator that only accepts "/accept/path".
    auto patterns = filepathOnlyPatterns();
    for (auto& p: patterns)
    {
        p.validator = [](std::string const& matchStr) -> bool {
            return matchStr.find("accept") != std::string::npos;
        };
    }

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(50) }, patterns, HintAction::Open);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "/accept/path");
}

TEST_CASE("HintModeHandler.NoValidatorPassesAll", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "see /foo/bar and /baz/qux" };

    // No validator set — both paths should pass through.
    handler.activate(
        lines, PageSize { LineCount(1), ColumnCount(40) }, filepathOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());

    auto foundFoo = false;
    auto foundBaz = false;
    for (auto const& m: handler.matches())
    {
        if (m.matchedText == "/foo/bar")
            foundFoo = true;
        if (m.matchedText == "/baz/qux")
            foundBaz = true;
    }
    CHECK(foundFoo);
    CHECK(foundBaz);
}

TEST_CASE("HintModeHandler.BareFilenameWithValidatedPattern", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Simulate bare filenames, extensionless files, and directories.
    auto lines = std::vector<std::string> { "error in main.cpp and README.md also Makefile and src" };

    // Create a filepath pattern with the broader regex (includes bare name branch)
    // and a validator that accepts specific names.
    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "filepath",
            .regex = std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                                std::regex_constants::ECMAScript | std::regex_constants::optimize),
            .validator = [](std::string const& matchStr) -> bool {
                // Simulate: these entries exist on disk, anything else doesn't.
                return matchStr == "main.cpp" || matchStr == "README.md" || matchStr == "Makefile"
                       || matchStr == "src";
            },
        },
    };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, patterns, HintAction::Open);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 4);
    CHECK(handler.matches()[0].matchedText == "main.cpp");
    CHECK(handler.matches()[1].matchedText == "README.md");
    CHECK(handler.matches()[2].matchedText == "Makefile");
    CHECK(handler.matches()[3].matchedText == "src");
}

TEST_CASE("HintModeHandler.BareFilenameFilteredByValidator", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Version numbers, domain names, and non-existent bare words should be filtered
    // when the validator confirms they don't exist on disk.
    auto lines = std::vector<std::string> { "version v0.6.3 and example.com and real.txt also build" };

    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "filepath",
            .regex = std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                                std::regex_constants::ECMAScript | std::regex_constants::optimize),
            .validator = [](std::string const& matchStr) -> bool {
                // Only real.txt "exists".
                return matchStr == "real.txt";
            },
        },
    };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, patterns, HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "real.txt");
}

TEST_CASE("HintModeHandler.SingleCharTokensNotMatched", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Single-character tokens are below the 2-char minimum of the broad regex branch.
    auto lines = std::vector<std::string> { "a b c d" };

    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "filepath",
            .regex = std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                                std::regex_constants::ECMAScript | std::regex_constants::optimize),
            .validator = [](std::string const&) -> bool { return true; }, // Accept everything.
        },
    };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, patterns, HintAction::Copy);

    REQUIRE(handler.isActive());
    CHECK(handler.matches().empty());
}

TEST_CASE("HintModeHandler.BuiltinRegexDoesNotMatchBareFilenames", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // With the default builtin patterns (no validator, no broad regex),
    // bare filenames must NOT be matched — they need a path separator.
    auto lines = std::vector<std::string> { "edit main.cpp and README.md" };

    handler.activate(
        lines, PageSize { LineCount(1), ColumnCount(40) }, filepathOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    // No filepath matches because there are no slashes.
    CHECK(handler.matches().empty());
}

TEST_CASE("extractPathFromFileUrl.NonFileUrl", "[hintmode]")
{
    CHECK(extractPathFromFileUrl("https://example.com") == "https://example.com");
    CHECK(extractPathFromFileUrl("ftp://server/file") == "ftp://server/file");
    CHECK(extractPathFromFileUrl("") == "");
    CHECK(extractPathFromFileUrl("/plain/path") == "/plain/path");
}

TEST_CASE("extractPathFromFileUrl.FileUrlWithLocalPath", "[hintmode]")
{
    CHECK(extractPathFromFileUrl("file:///home/user/file.txt") == "/home/user/file.txt");
    CHECK(extractPathFromFileUrl("file:///") == "/");
}

TEST_CASE("extractPathFromFileUrl.FileUrlWithHost", "[hintmode]")
{
    CHECK(extractPathFromFileUrl("file://hostname/home/user/file.txt") == "/home/user/file.txt");
    CHECK(extractPathFromFileUrl("file://hostname") == "");
}

TEST_CASE("HintModeHandler.CwdRelativeFilesystemValidation", "[hintmode]")
{
    namespace fs = std::filesystem;

    // Create a temporary directory with real filesystem entries.
    auto const tmpDir = fs::temp_directory_path() / "contour-hintmode-test";
    fs::create_directories(tmpDir / "src");
    std::ofstream(tmpDir / "Makefile").put('\n');
    std::ofstream(tmpDir / "main.cpp").put('\n');
    std::ofstream(tmpDir / "README.md").put('\n');
    std::ofstream(tmpDir / ".hidden").put('\n');

    auto const cwd = tmpDir.string();

    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Simulate terminal output containing a mix of existing and non-existing bare names.
    auto lines = std::vector<std::string> {
        "edit main.cpp and README.md also Makefile and src and .hidden but not bogus or phantom.xyz",
    };

    // Mirror the production validator from Terminal::activateHintMode:
    // resolve bare names relative to CWD, then check filesystem existence.
    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "filepath",
            .regex = std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                                std::regex_constants::ECMAScript | std::regex_constants::optimize),
            .validator = [cwd](std::string const& matchStr) -> bool {
                auto resolved = std::string {};
                if (matchStr.starts_with("/"))
                    resolved = matchStr;
                else
                    resolved = cwd + "/" + matchStr;
                return fs::exists(resolved);
            },
        },
    };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(100) }, patterns, HintAction::Open);

    REQUIRE(handler.isActive());

    // Collect matched text for easy assertion.
    auto matchedTexts = std::vector<std::string>();
    for (auto const& m: handler.matches())
        matchedTexts.push_back(m.matchedText);

    // Files and directories that exist in the temp CWD must be matched.
    CHECK(std::ranges::find(matchedTexts, "main.cpp") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, "README.md") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, "Makefile") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, "src") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, ".hidden") != matchedTexts.end());

    // Non-existent names must be filtered out by the validator.
    CHECK(std::ranges::find(matchedTexts, "bogus") == matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, "phantom.xyz") == matchedTexts.end());

    // Clean up.
    fs::remove_all(tmpDir);
}

TEST_CASE("HintModeHandler.HiddenFilesWithValidatedPattern", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Bare dotfiles like .gitignore, .bashrc, .config should be matched.
    auto lines = std::vector<std::string> {
        "check .gitignore and .bashrc also .config and README.md",
    };

    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "filepath",
            .regex = std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                                std::regex_constants::ECMAScript | std::regex_constants::optimize),
            .validator = [](std::string const& matchStr) -> bool {
                // Simulate: all dotfiles and README.md exist on disk.
                return matchStr == ".gitignore" || matchStr == ".bashrc" || matchStr == ".config"
                       || matchStr == "README.md";
            },
        },
    };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, patterns, HintAction::Open);

    REQUIRE(handler.isActive());

    auto matchedTexts = std::vector<std::string>();
    for (auto const& m: handler.matches())
        matchedTexts.push_back(m.matchedText);

    CHECK(std::ranges::find(matchedTexts, ".gitignore") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, ".bashrc") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, ".config") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, "README.md") != matchedTexts.end());
}

TEST_CASE("HintModeHandler.DotPrefixedRelativePath", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Dot-prefixed relative paths like .config/settings and .local/bin/tool
    // should be matched via alternative 3 of the broadened regex.
    auto lines = std::vector<std::string> {
        "open .config/settings and .local/bin/tool",
    };

    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "filepath",
            .regex = std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                                std::regex_constants::ECMAScript | std::regex_constants::optimize),
            .validator = [](std::string const&) -> bool { return true; }, // Accept everything.
        },
    };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(50) }, patterns, HintAction::Open);

    REQUIRE(handler.isActive());

    auto matchedTexts = std::vector<std::string>();
    for (auto const& m: handler.matches())
        matchedTexts.push_back(m.matchedText);

    CHECK(std::ranges::find(matchedTexts, ".config/settings") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, ".local/bin/tool") != matchedTexts.end());
}

TEST_CASE("HintModeHandler.IPv6FullAddress", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "address 2001:0db8:85a3:0000:0000:8a2e:0370:7334 here" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "2001:0db8:85a3:0000:0000:8a2e:0370:7334");
}

TEST_CASE("HintModeHandler.IPv6CompressedMiddle", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "link-local fe80::4117:f059:6f05:b06 on eth0" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(60) }, ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "fe80::4117:f059:6f05:b06");
}

TEST_CASE("HintModeHandler.IPv6CompressedStart", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "loopback ::1 and ::ffff:abcd more" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(50) }, ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 2);
    CHECK(handler.matches()[0].matchedText == "::1");
    CHECK(handler.matches()[1].matchedText == "::ffff:abcd");
}

TEST_CASE("HintModeHandler.IPv6CompressedEnd", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "prefix fe80:: in use" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "fe80::");
}

TEST_CASE("HintModeHandler.IPv6ShortCompressed", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "dns 2001:db8::1 server" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "2001:db8::1");
}

TEST_CASE("HintModeHandler.IPv6DoesNotMatchCppScope", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "std::vector and boost::asio and Foo::Bar" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(50) }, ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    CHECK(handler.matches().empty());
}

TEST_CASE("HintModeHandler.IPv6DoesNotMatchPlainHex", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "hash abcdef0123 and word deadbeef" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(40) }, ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    CHECK(handler.matches().empty());
}

// --- Unicode / non-ASCII offset tests ---

TEST_CASE("HintModeHandler.UnicodeOffsetInPrompt", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // The prompt symbol ❯ (U+276F) is 3 bytes in UTF-8 but occupies 1 grid column.
    // "❯ " = columns 0-1, URL starts at column 2.
    // In UTF-8 bytes: ❯ = 3 bytes, space = 1 byte → URL starts at byte 4.
    // Without the fix, startCol would incorrectly be 4 instead of 2.
    auto lines = std::vector<std::string> { "\xe2\x9d\xaf https://example.com" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(40) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://example.com");
    CHECK(handler.matches()[0].start.column == ColumnOffset(2));
    CHECK(handler.matches()[0].end.column == ColumnOffset(20));
}

TEST_CASE("HintModeHandler.AsciiPositionsUnchanged", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Pure ASCII: byte offset == column offset. Regression guard.
    auto lines = std::vector<std::string> { "visit https://example.com for more" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(40) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://example.com");
    CHECK(handler.matches()[0].start.column == ColumnOffset(6));
    CHECK(handler.matches()[0].end.column == ColumnOffset(24));
}

TEST_CASE("HintModeHandler.WideCharacterOffset", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // CJK character 中 (U+4E2D) is 3 bytes in UTF-8 and occupies 2 terminal columns.
    // Line::toUtf8() represents a wide character as the glyph in the leading cell
    // plus a space for the continuation cell. We therefore model the output as
    // "中 中  https://test.org":
    //   col 0: '中', col 1: continuation ' ', col 2: '中', col 3: continuation ' ',
    //   col 4: ' ' (separator), col 5..20: URL.
    auto lines = std::vector<std::string> { "\xe4\xb8\xad \xe4\xb8\xad  https://test.org" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(40) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://test.org");
    CHECK(handler.matches()[0].start.column == ColumnOffset(5));
    CHECK(handler.matches()[0].end.column == ColumnOffset(20));
}

TEST_CASE("HintModeHandler.MultipleUnicodeSegments", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // "→ https://a.com ★ https://b.com"
    // → (U+2192) = 3 bytes, ★ (U+2605) = 3 bytes
    // Columns: → = 0, ' ' = 1, URL1 starts at 2 (len 14, ends at 14),
    //          ' ' = 15, ★ = 16, ' ' = 17, URL2 starts at 18 (len 14, ends at 30)
    auto lines = std::vector<std::string> { "\xe2\x86\x92 https://a.com \xe2\x98\x85 https://b.com" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(50) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 2);
    CHECK(handler.matches()[0].matchedText == "https://a.com");
    CHECK(handler.matches()[0].start.column == ColumnOffset(2));
    CHECK(handler.matches()[0].end.column == ColumnOffset(14));
    CHECK(handler.matches()[1].matchedText == "https://b.com");
    CHECK(handler.matches()[1].start.column == ColumnOffset(18));
    CHECK(handler.matches()[1].end.column == ColumnOffset(30));
}

TEST_CASE("HintModeHandler.MatchAtLineStartWithUnicode", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // URL at column 0, followed by non-ASCII chars. Column 0 should be unaffected.
    // "https://start.org ❯" — URL at columns 0..17, then space at 18, ❯ at 19.
    auto lines = std::vector<std::string> { "https://start.org \xe2\x9d\xaf" };

    handler.activate(lines, PageSize { LineCount(1), ColumnCount(30) }, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://start.org");
    CHECK(handler.matches()[0].start.column == ColumnOffset(0));
    CHECK(handler.matches()[0].end.column == ColumnOffset(16));
}
