// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/HintModeHandler.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <set>

using namespace vtbackend;

namespace
{

/// Test executor that records callbacks.
class MockExecutor: public HintModeHandler::Executor
{
  public:
    std::string lastSelectedText;
    HintAction lastAction = HintAction::Copy;
    CellLocation lastStart {};
    CellLocation lastEnd {};
    int hintSelectedCount = 0;
    int hintEnteredCount = 0;
    int hintExitedCount = 0;
    int redrawCount = 0;

    void onHintSelected(std::string const& matchedText,
                        HintAction action,
                        CellLocation start,
                        CellLocation end) override
    {
        lastSelectedText = matchedText;
        lastAction = action;
        lastStart = start;
        lastEnd = end;
        ++hintSelectedCount;
    }

    void onHintModeEntered() override { ++hintEnteredCount; }
    void onHintModeExited() override { ++hintExitedCount; }
    void requestRedraw() override { ++redrawCount; }
};

/// The full set of built-in patterns, materialized on first use.
auto const& allPatterns()
{
    static auto const patterns = HintModeHandler::builtinPatterns();
    return patterns;
}

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

/// @p lines as unwrapped rows starting at grid line 0, every one of them labelable.
auto scanArea(std::vector<std::string> const& lines) -> HintScanArea
{
    auto area = HintScanArea {
        .rows = {},
        .labelableRows = HintRowRange { .first = LineOffset(0),
                                        .last = LineOffset::cast_from(lines.size()) - LineOffset(1) },
    };
    for (auto const i: std::views::iota(size_t { 0 }, lines.size()))
        area.rows.push_back(HintScanRow {
            .text = lines[i], .line = LineOffset::cast_from(i), .continuation = LineContinuation::No });
    return area;
}

/// @p lines as one wrapped logical line: the first row heads it, every later row continues it.
/// Starts at grid line @p firstLine, and every row is labelable.
auto wrappedScanArea(std::vector<std::string> const& lines, LineOffset firstLine = LineOffset(0))
    -> HintScanArea
{
    auto area = HintScanArea {
        .rows = {},
        .labelableRows =
            HintRowRange { .first = firstLine,
                           .last = firstLine + LineOffset::cast_from(lines.size()) - LineOffset(1) },
    };
    for (auto const i: std::views::iota(size_t { 0 }, lines.size()))
        area.rows.push_back(
            HintScanRow { .text = lines[i],
                          .line = firstLine + LineOffset::cast_from(i),
                          .continuation = i == 0 ? LineContinuation::No : LineContinuation::Yes });
    return area;
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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 3);
    CHECK(handler.matches()[0].label == "a");
    CHECK(handler.matches()[1].label == "b");
    CHECK(handler.matches()[2].label == "c");

    // Type 'b' — should filter to only match 'b' and auto-select.
    handler.processInput(U'b');

    CHECK(executor.hintSelectedCount == 1);
    CHECK(executor.lastSelectedText == "https://beta.com");
    // Verify forwarded coordinates: "https://beta.com" starts at column 18, ends at column 33.
    CHECK(executor.lastStart.line == LineOffset(0));
    CHECK(executor.lastStart.column == ColumnOffset(18));
    CHECK(executor.lastEnd.line == LineOffset(0));
    CHECK(executor.lastEnd.column == ColumnOffset(33));
    CHECK(!handler.isActive()); // Should have deactivated after selection.
}

TEST_CASE("HintModeHandler.EscapeCancels", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "https://example.com" };

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    CHECK(handler.matches().empty());
}

TEST_CASE("HintModeHandler.FilePathPattern", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "edit /home/user/file.txt and ./local/path" };

    handler.activate(scanArea(lines), allPatterns(), HintAction::Open);

    REQUIRE(handler.isActive());
    // Should find file paths.
    auto foundHome = false;
    auto foundLocal = false;
    for (auto const& m: handler.matches())
    {
        if (m.matchedText.contains("/home/user/file.txt"))
            foundHome = true;
        if (m.matchedText.contains("./local/path"))
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

    handler.activate(scanArea(lines), allPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), patterns, HintAction::Copy);

    REQUIRE(handler.matches().size() == 3);

    // Start typing but then backspace.
    handler.processInput(U'a');
    CHECK(!handler.isActive()); // 'a' is unique label -> auto-selected.

    // Reactivate for backspace test.
    handler.activate(scanArea(lines), patterns, HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Open);

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

    handler.activate(scanArea(lines), allPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), filepathOnlyPatterns(), HintAction::Open);

    REQUIRE(handler.isActive());

    auto foundTerminal = false;
    auto foundHelpers = false;
    for (auto const& m: handler.matches())
    {
        if (m.matchedText == "src/vtbackend/Terminal.cpp")
            foundTerminal = true;
        if (m.matchedText.contains("lib/utils/helpers.h"))
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

    handler.activate(scanArea(lines), filepathOnlyPatterns(), HintAction::Copy);

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
            return matchStr.contains("accept");
        };
    }

    handler.activate(scanArea(lines), patterns, HintAction::Open);

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
    handler.activate(scanArea(lines), filepathOnlyPatterns(), HintAction::Copy);

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
            .transformer = {},
        },
    };

    handler.activate(scanArea(lines), patterns, HintAction::Open);

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
            .transformer = {},
        },
    };

    handler.activate(scanArea(lines), patterns, HintAction::Copy);

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
            .transformer = {},
        },
    };

    handler.activate(scanArea(lines), patterns, HintAction::Copy);

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

    handler.activate(scanArea(lines), filepathOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    // No filepath matches because there are no slashes.
    CHECK(handler.matches().empty());
}

TEST_CASE("extractPathFromFileUrl.NonFileUrl", "[hintmode]")
{
    CHECK(extractPathFromFileUrl("https://example.com") == "https://example.com");
    CHECK(extractPathFromFileUrl("ftp://server/file") == "ftp://server/file");
    CHECK(extractPathFromFileUrl("").empty());
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
    CHECK(extractPathFromFileUrl("file://hostname").empty());
}

TEST_CASE("extractPathFromFileUrl.WindowsDriveLetter", "[hintmode]")
{
    // A Windows drive-letter authority must not be mistaken for a host and stripped.
    CHECK(extractPathFromFileUrl("file://C:/Users/user/file.txt") == "C:/Users/user/file.txt");
    // The standards-conformant form has an empty authority and a leading slash before the drive.
    CHECK(extractPathFromFileUrl("file:///C:/Users/user/file.txt") == "C:/Users/user/file.txt");
    // Lower-case drive letters are equally valid.
    CHECK(extractPathFromFileUrl("file://d:/temp/x") == "d:/temp/x");
}

TEST_CASE("extractPathFromFileUrl.WindowsDriveLetterWithHost", "[hintmode]")
{
    // OSC 7 on Windows commonly reports a real hostname *and* a drive-letter path
    // (e.g. "file://MYPC/C:/Users/user"). The leading slash before the drive letter
    // must still be stripped, otherwise callers get an invalid "/C:/..." path that
    // Windows CreateProcess() rejects with ERROR_DIRECTORY ("directory name is invalid").
    CHECK(extractPathFromFileUrl("file://hostname/C:/Users/user") == "C:/Users/user");
    CHECK(extractPathFromFileUrl("file://MYPC/d:/temp/x") == "d:/temp/x");
}

TEST_CASE("localWorkingDirectory.localHostIsOpenable", "[hintmode]")
{
    // The pane's own host: strip the scheme and authority down to a plain local path.
    CHECK(localWorkingDirectory("file://fedora/home/user/proj", "fedora") == "/home/user/proj");
    // Case-insensitively, and tolerant of a fully-qualified name on either side (same machine).
    CHECK(localWorkingDirectory("file://FEDORA/home/user", "fedora") == "/home/user");
    CHECK(localWorkingDirectory("file://fedora.corp.example/home/user", "fedora") == "/home/user");
    CHECK(localWorkingDirectory("file://fedora/home/user", "fedora.corp.example") == "/home/user");
    // An empty authority (file:///path) and an explicit "localhost" are this machine too.
    CHECK(localWorkingDirectory("file:///home/user", "fedora") == "/home/user");
    CHECK(localWorkingDirectory("file://localhost/home/user", "fedora") == "/home/user");
    // A bare path (a shell that emits OSC 7 without the file:// wrapper) is local as-is.
    CHECK(localWorkingDirectory("/home/user", "fedora") == "/home/user");
}

TEST_CASE("localWorkingDirectory.remoteHostIsRejected", "[hintmode]")
{
    // A different host is a remote (e.g. SSH) working directory: its path does not exist here.
    CHECK(localWorkingDirectory("file://remotehost/home/user", "fedora") == std::nullopt);
    CHECK(localWorkingDirectory("file://remotehost/C:/Users/user", "fedora") == std::nullopt);
    // A host with no path at all has nothing to open.
    CHECK(localWorkingDirectory("file://fedora", "fedora") == std::nullopt);
    // Nothing reported yet.
    CHECK(localWorkingDirectory("", "fedora") == std::nullopt);
}

TEST_CASE("localWorkingDirectory.windowsDriveLetterIsLocal", "[hintmode]")
{
    // A drive-letter authority is a path, not a host, so it is this machine's.
    CHECK(localWorkingDirectory("file://C:/Users/user", "laptop") == "C:/Users/user");
    CHECK(localWorkingDirectory("file:///C:/Users/user", "laptop") == "C:/Users/user");
    // A real host in front of a drive path is still local only when the host matches.
    CHECK(localWorkingDirectory("file://laptop/C:/Users/user", "laptop") == "C:/Users/user");
    CHECK(localWorkingDirectory("file://desktop/C:/Users/user", "laptop") == std::nullopt);
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
            .transformer = {},
        },
    };

    handler.activate(scanArea(lines), patterns, HintAction::Open);

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
            .transformer = {},
        },
    };

    handler.activate(scanArea(lines), patterns, HintAction::Open);

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
            .transformer = {},
        },
    };

    handler.activate(scanArea(lines), patterns, HintAction::Open);

    REQUIRE(handler.isActive());

    auto matchedTexts = std::vector<std::string>();
    for (auto const& m: handler.matches())
        matchedTexts.push_back(m.matchedText);

    CHECK(std::ranges::find(matchedTexts, ".config/settings") != matchedTexts.end());
    CHECK(std::ranges::find(matchedTexts, ".local/bin/tool") != matchedTexts.end());
}

TEST_CASE("HintModeHandler.TransformerRewritesMatchedText", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "open src/foo.cpp for editing" };

    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "filepath",
            .regex = std::regex(R"([\w./]+)", std::regex_constants::ECMAScript),
            .validator = [](std::string const& matchStr) -> bool { return matchStr.contains('/'); },
            .transformer = [](std::string const& matchStr) -> std::string { return "/project/" + matchStr; },
        },
    };

    handler.activate(scanArea(lines), patterns, HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);

    // The matched text should be transformed (absolute path), not the raw terminal text.
    CHECK(handler.matches()[0].matchedText == "/project/src/foo.cpp");

    // Selecting the hint should forward the transformed text.
    handler.processInput(U'a');
    CHECK(executor.hintSelectedCount == 1);
    CHECK(executor.lastSelectedText == "/project/src/foo.cpp");
}

TEST_CASE("HintModeHandler.IPv6FullAddress", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "address 2001:0db8:85a3:0000:0000:8a2e:0370:7334 here" };

    handler.activate(scanArea(lines), ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "2001:0db8:85a3:0000:0000:8a2e:0370:7334");
}

TEST_CASE("HintModeHandler.IPv6CompressedMiddle", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "link-local fe80::4117:f059:6f05:b06 on eth0" };

    handler.activate(scanArea(lines), ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "fe80::4117:f059:6f05:b06");
}

TEST_CASE("HintModeHandler.IPv6CompressedStart", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "loopback ::1 and ::ffff:abcd more" };

    handler.activate(scanArea(lines), ipv6OnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "fe80::");
}

TEST_CASE("HintModeHandler.IPv6ShortCompressed", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "dns 2001:db8::1 server" };

    handler.activate(scanArea(lines), ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "2001:db8::1");
}

TEST_CASE("HintModeHandler.IPv6DoesNotMatchCppScope", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "std::vector and boost::asio and Foo::Bar" };

    handler.activate(scanArea(lines), ipv6OnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    CHECK(handler.matches().empty());
}

TEST_CASE("HintModeHandler.IPv6DoesNotMatchPlainHex", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto lines = std::vector<std::string> { "hash abcdef0123 and word deadbeef" };

    handler.activate(scanArea(lines), ipv6OnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

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

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.isActive());
    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://start.org");
    CHECK(handler.matches()[0].start.column == ColumnOffset(0));
    CHECK(handler.matches()[0].end.column == ColumnOffset(16));
}

// {{{ Logical-line grouping and offset mapping

TEST_CASE("buildLogicalLines.UnwrappedRowsStayApart", "[hintmode]")
{
    auto const rows = std::vector<HintScanRow> {
        HintScanRow { .text = "one", .line = LineOffset(0), .continuation = LineContinuation::No },
        HintScanRow { .text = "two", .line = LineOffset(1), .continuation = LineContinuation::No },
    };

    auto const logical = buildLogicalLines(rows);

    REQUIRE(logical.size() == 2);
    CHECK(logical[0].text == "one");
    CHECK(logical[0].firstLine == LineOffset(0));
    CHECK(logical[1].text == "two");
    CHECK(logical[1].firstLine == LineOffset(1));
}

TEST_CASE("buildLogicalLines.ContinuationsJoinTheHead", "[hintmode]")
{
    auto const rows = std::vector<HintScanRow> {
        HintScanRow { .text = "head", .line = LineOffset(3), .continuation = LineContinuation::No },
        HintScanRow { .text = "-mid", .line = LineOffset(4), .continuation = LineContinuation::Yes },
        HintScanRow { .text = "-end", .line = LineOffset(5), .continuation = LineContinuation::Yes },
        HintScanRow { .text = "next", .line = LineOffset(6), .continuation = LineContinuation::No },
    };

    auto const logical = buildLogicalLines(rows);

    REQUIRE(logical.size() == 2);
    CHECK(logical[0].text == "head-mid-end");
    CHECK(logical[0].firstLine == LineOffset(3));
    CHECK(logical[0].rowCodepointEnds == std::vector<size_t> { 4, 8, 12 });
    CHECK(logical[1].text == "next");
    CHECK(logical[1].firstLine == LineOffset(6));
}

TEST_CASE("buildLogicalLines.LeadingContinuationHeadsItsOwnLine", "[hintmode]")
{
    // The head scrolled out of the scanned range: the continuation is all we can see of it, and
    // must not be dropped.
    auto const rows = std::vector<HintScanRow> {
        HintScanRow { .text = "tail", .line = LineOffset(0), .continuation = LineContinuation::Yes },
        HintScanRow { .text = "-end", .line = LineOffset(1), .continuation = LineContinuation::Yes },
    };

    auto const logical = buildLogicalLines(rows);

    REQUIRE(logical.size() == 1);
    CHECK(logical[0].text == "tail-end");
    CHECK(logical[0].firstLine == LineOffset(0));
}

TEST_CASE("buildLogicalLines.NonConsecutiveRowsBreakTheRun", "[hintmode]")
{
    // A gap in the offsets would break gridPositionOf()'s firstLine + rowIndex arithmetic.
    auto const rows = std::vector<HintScanRow> {
        HintScanRow { .text = "aaa", .line = LineOffset(0), .continuation = LineContinuation::No },
        HintScanRow { .text = "bbb", .line = LineOffset(7), .continuation = LineContinuation::Yes },
    };

    auto const logical = buildLogicalLines(rows);

    REQUIRE(logical.size() == 2);
    CHECK(logical[1].firstLine == LineOffset(7));
}

TEST_CASE("gridPositionOf.MapsAcrossRowBoundaries", "[hintmode]")
{
    auto const logical = HintLogicalLine {
        .text = "abcdefghi",
        .firstLine = LineOffset(-2),
        .rowCodepointEnds = { 3, 6, 9 },
    };

    // First row.
    CHECK(gridPositionOf(logical, 0) == CellLocation { .line = LineOffset(-2), .column = ColumnOffset(0) });
    CHECK(gridPositionOf(logical, 2) == CellLocation { .line = LineOffset(-2), .column = ColumnOffset(2) });
    // Second row starts exactly at the boundary.
    CHECK(gridPositionOf(logical, 3) == CellLocation { .line = LineOffset(-1), .column = ColumnOffset(0) });
    CHECK(gridPositionOf(logical, 5) == CellLocation { .line = LineOffset(-1), .column = ColumnOffset(2) });
    // Third row.
    CHECK(gridPositionOf(logical, 6) == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    CHECK(gridPositionOf(logical, 8) == CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
    // Past the end clamps to the last existing cell.
    CHECK(gridPositionOf(logical, 9) == CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
    CHECK(gridPositionOf(logical, 99) == CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
}

TEST_CASE("gridPositionOf.EmptyLogicalLineClampsToItsFirstCell", "[hintmode]")
{
    auto const logical =
        HintLogicalLine { .text = "", .firstLine = LineOffset(4), .rowCodepointEnds = { 0 } };

    CHECK(gridPositionOf(logical, 0) == CellLocation { .line = LineOffset(4), .column = ColumnOffset(0) });
}

// }}}

// {{{ Wrapped-line matching

TEST_CASE("HintModeHandler.UrlWrappedAcrossTwoRows", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // "https://example.com/some/very/long/path" broken after column 20.
    auto const lines = std::vector<std::string> { "https://example.com/", "some/very/long/path" };

    handler.activate(wrappedScanArea(lines), urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    auto const& match = handler.matches()[0];
    CHECK(match.matchedText == "https://example.com/some/very/long/path");
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    CHECK(match.end == CellLocation { .line = LineOffset(1), .column = ColumnOffset(18) });
}

TEST_CASE("HintModeHandler.UrlWrappedAcrossThreeRows", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto const lines = std::vector<std::string> { "https://example.com/a/", "middle/part/", "tail" };

    handler.activate(wrappedScanArea(lines), urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    auto const& match = handler.matches()[0];
    CHECK(match.matchedText == "https://example.com/a/middle/part/tail");
    CHECK(match.start == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    CHECK(match.end == CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) });
}

TEST_CASE("HintModeHandler.UnwrappedRowsDoNotBleedIntoEachOther", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Same text as the two-row wrap case, but the rows are separate logical lines: the second row
    // is not a URL at all, so only the first must match.
    auto const lines = std::vector<std::string> { "https://example.com/", "some/very/long/path" };

    handler.activate(scanArea(lines), urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://example.com/");
    CHECK(handler.matches()[0].end.line == LineOffset(0));
}

TEST_CASE("HintModeHandler.SelectionForwardsAWrappedRange", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    auto const lines = std::vector<std::string> { "https://example.com/", "wrapped" };

    handler.activate(wrappedScanArea(lines), urlOnlyPatterns(), HintAction::Select);
    REQUIRE(handler.matches().size() == 1);

    handler.processInput(U'a');

    CHECK(executor.hintSelectedCount == 1);
    CHECK(executor.lastAction == HintAction::Select);
    CHECK(executor.lastSelectedText == "https://example.com/wrapped");
    CHECK(executor.lastStart == CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    CHECK(executor.lastEnd == CellLocation { .line = LineOffset(1), .column = ColumnOffset(6) });
}

TEST_CASE("HintModeHandler.OverlapIsRejectedAcrossARowBoundary", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // The URL wraps, and its tail rows also look like file paths. Only the longest match at each
    // position survives, and the nested path matches inside the URL must not reappear as hints.
    auto const lines = std::vector<std::string> { "https://example.com/", "src/vtbackend/x.h" };

    handler.activate(wrappedScanArea(lines), allPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://example.com/src/vtbackend/x.h");
}

// }}}

// {{{ Labelable range

TEST_CASE("HintModeHandler.MatchStartingOutsideTheLabelableRangeIsNotOffered", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Row 0 is scanned for its text but cannot carry a label, so the URL that starts there — and
    // wraps into the labelable row 1 — is not offered at all.
    auto area = wrappedScanArea(std::vector<std::string> { "https://example.com/", "wrapped" });
    area.labelableRows = HintRowRange { .first = LineOffset(1), .last = LineOffset(1) };

    handler.activate(area, urlOnlyPatterns(), HintAction::Copy);

    CHECK(handler.matches().empty());
}

TEST_CASE("HintModeHandler.MatchEndingOutsideTheLabelableRangeIsOfferedInFull", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // The mirror case: the label lands on the visible row 0, and row 1 is scanned only to complete
    // the text. The hint is offered, and the text it yields is whole.
    auto area = wrappedScanArea(std::vector<std::string> { "https://example.com/", "wrapped" });
    area.labelableRows = HintRowRange { .first = LineOffset(0), .last = LineOffset(0) };

    handler.activate(area, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "https://example.com/wrapped");
    CHECK(handler.matches()[0].end.line == LineOffset(1));
}

TEST_CASE("HintModeHandler.HistoryRowOffsetsRoundTrip", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Grid rows above the page carry negative offsets; they must survive scanning unchanged.
    auto const area =
        wrappedScanArea(std::vector<std::string> { "see https://example.com/", "tail" }, LineOffset(-4));

    handler.activate(area, urlOnlyPatterns(), HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].start == CellLocation { .line = LineOffset(-4), .column = ColumnOffset(4) });
    CHECK(handler.matches()[0].end == CellLocation { .line = LineOffset(-3), .column = ColumnOffset(3) });
}

// }}}

// {{{ Label widths

namespace
{
/// One URL per match, spread over @p count single-row logical lines.
auto linesWithUrls(size_t count) -> std::vector<std::string>
{
    auto lines = std::vector<std::string>();
    lines.reserve(count);
    for (auto const i: std::views::iota(size_t { 0 }, count))
        lines.push_back(std::format("https://site{}.example", i));
    return lines;
}
} // namespace

TEST_CASE("HintModeHandler.LabelWidthGrowsWithTheMatchCount", "[hintmode]")
{
    struct TestCase
    {
        size_t matchCount;
        size_t expectedWidth;
    };

    // 26 labels per character: 26 fit in one, 676 in two, and beyond that three are needed. Before
    // the base-26 generalization, index 676 produced 'a' + 26 == '{' — a label no keystroke could
    // ever select.
    auto const cases = std::vector<TestCase> {
        TestCase { .matchCount = 1, .expectedWidth = 1 },
        TestCase { .matchCount = 26, .expectedWidth = 1 },
        TestCase { .matchCount = 27, .expectedWidth = 2 },
        TestCase { .matchCount = 676, .expectedWidth = 2 },
        TestCase { .matchCount = 677, .expectedWidth = 3 },
    };

    for (auto const& testCase: cases)
    {
        INFO(std::format("{} matches", testCase.matchCount));
        auto executor = MockExecutor {};
        auto handler = HintModeHandler { executor };

        handler.activate(scanArea(linesWithUrls(testCase.matchCount)), urlOnlyPatterns(), HintAction::Copy);

        auto const& matches = handler.matches();
        REQUIRE(matches.size() == testCase.matchCount);

        auto seen = std::set<std::string> {};
        for (auto const& match: matches)
        {
            CHECK(match.label.size() == testCase.expectedWidth);
            // Every character must be one hint mode accepts, and every label must be unique.
            for (auto const ch: match.label)
                CHECK((ch >= 'a' && ch <= 'z'));
            CHECK(seen.insert(match.label).second);
        }
    }
}

TEST_CASE("HintModeHandler.ThreeCharLabelIsSelectable", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    handler.activate(scanArea(linesWithUrls(677)), urlOnlyPatterns(), HintAction::Copy);
    REQUIRE(handler.matches().size() == 677);

    // The last match gets the highest label; typing it must select that match and nothing else.
    auto const label = handler.matches().back().label;
    auto const expectedText = handler.matches().back().matchedText;
    REQUIRE(label.size() == 3);

    for (auto const ch: label)
        handler.processInput(static_cast<char32_t>(ch));

    CHECK(executor.hintSelectedCount == 1);
    CHECK(executor.lastSelectedText == expectedText);
    CHECK(!handler.isActive());
}

// }}}

// {{{ Input handling paths a single-character label set cannot reach

TEST_CASE("HintModeHandler.BackspaceRemovesATypedCharacter", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // More than 26 matches, so labels are two characters and one keystroke cannot auto-select --
    // which is the only way a non-empty filter can still be there when Backspace arrives.
    handler.activate(scanArea(linesWithUrls(30)), urlOnlyPatterns(), HintAction::Copy);
    REQUIRE(handler.matches().size() == 30);

    handler.processInput(U'a');
    REQUIRE(handler.isActive());
    REQUIRE(handler.currentFilter() == "a");
    auto const narrowed = handler.matches().size();
    CHECK(narrowed < 30); // 'a' filtered to the "a?" labels only.

    handler.processInput(U'\x08');

    CHECK(handler.isActive());
    CHECK(handler.currentFilter().empty());
    CHECK(handler.matches().size() == 30); // every candidate is back
}

TEST_CASE("HintModeHandler.NonAlphabeticInputIsIgnored", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    handler.activate(scanArea(linesWithUrls(3)), urlOnlyPatterns(), HintAction::Copy);
    REQUIRE(handler.matches().size() == 3);

    // Consumed so it cannot leak to the running application, but it changes nothing.
    CHECK(handler.processInput(U'1'));
    CHECK(handler.processInput(U'-'));

    CHECK(handler.isActive());
    CHECK(handler.currentFilter().empty());
    CHECK(executor.hintSelectedCount == 0);
}

TEST_CASE("HintModeHandler.AKeyMatchingNoLabelExitsHintMode", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    handler.activate(scanArea(linesWithUrls(3)), urlOnlyPatterns(), HintAction::Copy);
    REQUIRE(handler.matches().size() == 3); // labels a, b, c

    handler.processInput(U'z');

    CHECK(!handler.isActive());
    CHECK(executor.hintSelectedCount == 0);
    CHECK(executor.hintExitedCount == 1);
}

TEST_CASE("HintModeHandler.InputAndDeactivationAreNoOpsWhenInactive", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Never activated: input is not consumed, so the caller passes it on.
    CHECK_FALSE(handler.processInput(U'a'));

    handler.activate(scanArea(linesWithUrls(1)), urlOnlyPatterns(), HintAction::Copy);
    handler.deactivate();
    REQUIRE(executor.hintExitedCount == 1);

    // Deactivating twice must not fire the exit callback again -- the Escape key and an
    // auto-selection can both arrive at deactivate().
    handler.deactivate();
    CHECK(executor.hintExitedCount == 1);
}

TEST_CASE("HintModeHandler.LongestMatchWinsAtTheSameStart", "[hintmode]")
{
    auto executor = MockExecutor {};
    auto handler = HintModeHandler { executor };

    // Two patterns that match at the very same position with different lengths. The sort's
    // tie-break puts the longer one first, and the overlap pass then drops the shorter -- the rule
    // the wrapped-line overlap comparison relies on.
    auto patterns = std::vector<HintPattern> {
        HintPattern {
            .name = "short", .regex = std::regex(R"([0-9a-f]{7})"), .validator = {}, .transformer = {} },
        HintPattern {
            .name = "long", .regex = std::regex(R"([0-9a-f]{10})"), .validator = {}, .transformer = {} },
    };

    handler.activate(scanArea(std::vector<std::string> { "abcdef0123" }), patterns, HintAction::Copy);

    REQUIRE(handler.matches().size() == 1);
    CHECK(handler.matches()[0].matchedText == "abcdef0123");
}

// }}}
