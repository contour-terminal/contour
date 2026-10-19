// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkMatcher.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;

namespace
{
ScanText plain(std::string_view text)
{
    auto rows = std::vector<ScanRow> { makePlainScanRow(text) };
    return ScanText { rows, ScanClip {} };
}

std::vector<LinkCandidate> clickable(std::string_view text)
{
    return findLinks(
        plain(text), *LinkPatterns::builtins(), PatternSelection::ClickableOnly, LinkOverlap::Merge);
}

struct Expected
{
    std::string_view printed;
    std::string_view text;   // the decorated span
    std::string_view target; // what opens
    LinkKind kind;
    std::optional<LinkLocation> location;
};

// §4.3's table, plus the neighbours a reviewer would try next.
constexpr auto Cases = std::array {
    Expected { "https://en.wikipedia.org/wiki/Foo_(bar)",
               "https://en.wikipedia.org/wiki/Foo_(bar)",
               "https://en.wikipedia.org/wiki/Foo_(bar)",
               LinkKind::Url,
               std::nullopt },
    Expected { "(see https://example.com/a).",
               "https://example.com/a",
               "https://example.com/a",
               LinkKind::Url,
               std::nullopt },
    Expected { "src/foo.cpp:12:5: error",
               "src/foo.cpp:12:5",
               "src/foo.cpp",
               LinkKind::Path,
               LinkLocation { .line = 12, .column = 5 } },
    Expected { "C:\\src\\a.txt(12,5): warning",
               "C:\\src\\a.txt(12,5)",
               "C:\\src\\a.txt",
               LinkKind::Path,
               LinkLocation { .line = 12, .column = 5 } },
    Expected { "README.md.", "README.md", "README.md", LinkKind::Path, std::nullopt },
    Expected { "<https://x.y/z>", "https://x.y/z", "https://x.y/z", LinkKind::Url, std::nullopt },
    Expected { "\"https://x.y/z\"", "https://x.y/z", "https://x.y/z", LinkKind::Url, std::nullopt },
    Expected { "[docs](https://x.y/a_(b)) here",
               "https://x.y/a_(b)",
               "https://x.y/a_(b)",
               LinkKind::Url,
               std::nullopt },
    Expected { "https://x.y/?a=b&c=d!",
               "https://x.y/?a=b&c=d",
               "https://x.y/?a=b&c=d",
               LinkKind::Url,
               std::nullopt },
    Expected { "mailto:me@x.y,", "mailto:me@x.y", "mailto:me@x.y", LinkKind::Url, std::nullopt },
    Expected { "file:///tmp/x", "file:///tmp/x", "file:///tmp/x", LinkKind::Url, std::nullopt },
    Expected { "--file=src/x.cpp", "src/x.cpp", "src/x.cpp", LinkKind::Path, std::nullopt },
    Expected { "(also /etc/hosts)", "/etc/hosts", "/etc/hosts", LinkKind::Path, std::nullopt },
    Expected { "build/out/", "build/out/", "build/out/", LinkKind::Path, std::nullopt },
    Expected { "x.cpp(7)", "x.cpp(7)", "x.cpp", LinkKind::Path, LinkLocation { .line = 7, .column = 1 } },
};
} // namespace

TEST_CASE("LinkMatcher.tableOfPrintedLinks", "[links]")
{
    for (auto const& expected: Cases)
    {
        INFO(expected.printed);
        // A bare word ("see", "--file") is a path candidate too, so the expected link is looked up by its
        // decorated text rather than taken as the first candidate.
        auto const found = clickable(expected.printed);
        auto const link = std::ranges::find(found, std::string { expected.text }, &LinkCandidate::text);
        REQUIRE(link != found.end());
        CHECK(link->target == expected.target);
        CHECK(link->kind == expected.kind);
        CHECK(link->location == expected.location);
        // Every case is ASCII, one cell per byte: the decorated span's cells are where `text` sits in
        // `printed`.
        auto const column = expected.printed.find(expected.text);
        REQUIRE(column != std::string_view::npos);
        CHECK(link->first == ScanPosition { .row = 0, .column = ColumnOffset(static_cast<int>(column)) });
        CHECK(link->last
              == ScanPosition {
                  .row = 0, .column = ColumnOffset(static_cast<int>(column + expected.text.size()) - 1) });
    }
}

TEST_CASE("LinkMatcher.everyWordOfAListingIsACandidate", "[links]")
{
    auto const found = clickable("README.md  src/  ~/notes.md  ./build");
    REQUIRE(found.size() == 4);
    CHECK(found[1].target == "src/");
    CHECK(found[2].target == "~/notes.md");
    CHECK(found[3].target == "./build");
}

TEST_CASE("LinkMatcher.mergeKeepsTheEarliestLongest", "[links]")
{
    // A path inside a URL's query is not a second link.
    auto const merged = clickable("https://x.y/?f=a/b");
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].kind == LinkKind::Url);

    // Unmerged: the URL, the bare word "https" at the line start, and "a/b" after '='.
    auto const kept = findLinks(plain("https://x.y/?f=a/b"),
                                *LinkPatterns::builtins(),
                                PatternSelection::ClickableOnly,
                                LinkOverlap::Keep);
    REQUIRE(kept.size() == 3);
    // Unsorted: pattern order first (the URL), then position within a pattern.
    CHECK(kept[0].kind == LinkKind::Url);
    CHECK(kept[1].byteBegin < kept[2].byteBegin);
    CHECK(kept[1].kind == LinkKind::Path);
}

TEST_CASE("LinkMatcher.hintOnlyPatternsNeedAllSelection", "[links]")
{
    CHECK(clickable("deadbeef12").empty() == false); // a bare name is still a path candidate
    CHECK(std::ranges::none_of(clickable("deadbeef12"),
                               [](LinkCandidate const& c) { return c.kind == LinkKind::Custom; }));
    auto const all =
        findLinks(plain("deadbeef12"), *LinkPatterns::builtins(), PatternSelection::All, LinkOverlap::Keep);
    CHECK(std::ranges::any_of(all, [](LinkCandidate const& c) { return c.kind == LinkKind::Custom; }));
}

TEST_CASE("LinkMatcher.emptyMatchesNeitherLoopNorReport", "[links]")
{
    // A hint-only pattern may match the empty string (only a clickable one is rejected at compile time): the
    // scan must terminate and report the non-empty matches only.
    auto const entries = std::array { LinkPatternSpec {
        .name = "maybe-x", .regex = "x*", .activation = LinkActivation::HintOnly } };
    auto const table = LinkPatterns::compile(entries);
    REQUIRE(table.find("maybe-x") != nullptr);
    auto const found = findLinks(plain("ab xx c"), table, PatternSelection::All, LinkOverlap::Keep);
    auto const mine = std::ranges::count_if(
        found, [](LinkCandidate const& c) { return c.kind == LinkKind::Custom && c.text == "xx"; });
    CHECK(mine == 1);
    CHECK(std::ranges::none_of(found, [](LinkCandidate const& c) { return c.byteEnd <= c.byteBegin; }));
}

TEST_CASE("LinkMatcher.customPatternOpensItsTemplate", "[links]")
{
    auto const entries =
        std::array { LinkPatternSpec { .name = "jira",
                                       .regex = R"(\b[A-Z]+-\d+\b)",
                                       .activation = LinkActivation::Clickable,
                                       .urlTemplate = "https://jira.example.com/browse/{0}" } };
    auto const table = LinkPatterns::compile(entries);
    auto const found =
        findLinks(plain("fixed in ABC-123."), table, PatternSelection::ClickableOnly, LinkOverlap::Merge);
    auto const jira = std::ranges::find(found, LinkKind::Custom, &LinkCandidate::kind);
    REQUIRE(jira != found.end());
    CHECK(jira->text == "ABC-123");
    CHECK(jira->target == "https://jira.example.com/browse/ABC-123");
}

TEST_CASE("LinkMatcher.aTemplateReadsGroupsFromThePrintedText", "[links]")
{
    // Groups as printed, encoded where a URL cannot hold them; a group that took no part inserts nothing.
    auto const entries = std::array { LinkPatternSpec { .name = "ticket",
                                                        .regex = R"(#([^\s@]+)(?:@(\w+))?)",
                                                        .activation = LinkActivation::Clickable,
                                                        .urlTemplate = "https://t/{1}?at={2}" } };
    auto const table = LinkPatterns::compile(entries);
    auto const found =
        findLinks(plain("see #ä12@main and #x9"), table, PatternSelection::All, LinkOverlap::Keep);
    auto const custom = [&](std::string_view text) {
        return std::ranges::find(found, text, &LinkCandidate::text);
    };
    REQUIRE(custom("#ä12@main") != found.end());
    CHECK(custom("#ä12@main")->target == "https://t/ä12?at=main");
    REQUIRE(custom("#x9") != found.end());
    CHECK(custom("#x9")->target == "https://t/x9?at=");
}

TEST_CASE("LinkMatcher.aCustomMatchGoesBeforeABareFileNameOfTheSameSpan", "[links]")
{
    auto const entries = std::array { LinkPatternSpec { .name = "ticket",
                                                        .regex = R"(\bTKT-\d+\b)",
                                                        .activation = LinkActivation::Clickable,
                                                        .urlTemplate = "https://t/{0}" } };
    auto const found = findLinks(
        plain("TKT-7"), LinkPatterns::compile(entries), PatternSelection::ClickableOnly, LinkOverlap::Merge);
    REQUIRE(found.size() == 1);
    CHECK(found[0].kind == LinkKind::Custom);
    CHECK(found[0].target == "https://t/TKT-7");
}

TEST_CASE("LinkMatcher.aUrlWinsOverACustomMatchInsideIt", "[links]")
{
    auto const entries = std::array { LinkPatternSpec { .name = "jira",
                                                        .regex = R"(\b[A-Z]+-\d+\b)",
                                                        .activation = LinkActivation::Clickable,
                                                        .urlTemplate = "https://j/{0}" } };
    auto const found = findLinks(plain("https://jira.example.com/browse/ABC-123"),
                                 LinkPatterns::compile(entries),
                                 PatternSelection::ClickableOnly,
                                 LinkOverlap::Merge);
    REQUIRE(found.size() == 1);
    CHECK(found[0].kind == LinkKind::Url);
}

TEST_CASE("LinkMatcher.matchTouchingAClippedEdgeIsDropped", "[links]")
{
    auto rows = std::vector<ScanRow> { makePlainScanRow("https://x.y/long") };
    auto const clipped = ScanText { rows, ScanClip { .front = ScanEdge::Whole, .back = ScanEdge::Clipped } };
    auto const found =
        findLinks(clipped, *LinkPatterns::builtins(), PatternSelection::ClickableOnly, LinkOverlap::Merge);
    CHECK(std::ranges::none_of(found, [](LinkCandidate const& c) { return c.kind == LinkKind::Url; }));
}

TEST_CASE("LinkMatcher.wideAndClusterCellsMapToTheirColumns", "[links]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(60) } };
    // 東京 takes columns 0-3; é is one cell (e + U+0301); the URL starts at column 7. The fullwidth full stop
    // after パス ends it.
    mock.writeToScreen("東京 e\xCC\x81 https://例え.jp/パス。次へ");
    auto rows = std::vector<ScanRow> { makeScanRow(
        mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)), LineOffset(0), LineContinuation::No) };
    auto const text = ScanText { rows, ScanClip {} };
    auto const found =
        findLinks(text, *LinkPatterns::builtins(), PatternSelection::ClickableOnly, LinkOverlap::Merge);
    auto const url = std::ranges::find(found, LinkKind::Url, &LinkCandidate::kind);
    REQUIRE(url != found.end());
    CHECK(url->target == "https://例え.jp/パス");
    CHECK(url->first.column == ColumnOffset(7));
    // https:// (8) + 例え (4 columns) + .jp/ (4) + パス (4 columns) = 20 columns: 7..26.
    CHECK(url->last.column == ColumnOffset(26));
}

TEST_CASE("LinkMatcher.aPlainRowEndsAUrlWhereTheGridRowDoes", "[links]")
{
    // What the settings page's preview matches: the fullwidth full stop after パス ends the URL there, as it
    // does in the terminal (wideAndClusterCellsMapToTheirColumns above).
    auto const found = findLinks(plain("https://例え.jp/パス。次へ"),
                                 *LinkPatterns::builtins(),
                                 PatternSelection::ClickableOnly,
                                 LinkOverlap::Merge);
    auto const url = std::ranges::find(found, LinkKind::Url, &LinkCandidate::kind);
    REQUIRE(url != found.end());
    CHECK(url->target == "https://例え.jp/パス");
}

TEST_CASE("LinkMatcher.printedTextMayChooseOnlyABuiltInUrlScheme", "[links]")
{
    // §7.6: built-in URL detection opens http, https, ftp, mailto and file only. Where a user pattern's link
    // takes its scheme from the printed text -- no url, or one that does not start with a scheme of its own
    // -- the same holds, or the match is no link. A scheme the user's template writes is the user's choice.
    auto const docs = [](std::optional<std::string> url) {
        auto const specs = std::array { LinkPatternSpec { .name = "doc",
                                                          .regex = R"(\S+\.pdf)",
                                                          .activation = LinkActivation::Clickable,
                                                          .urlTemplate = std::move(url) } };
        return LinkPatterns::compile(specs);
    };
    auto const linksOf = [](LinkPatterns const& patterns, std::string_view text) {
        auto targets = std::vector<std::string> {};
        for (auto const& candidate:
             findLinks(plain(text), patterns, PatternSelection::ClickableOnly, LinkOverlap::Keep))
            if (candidate.kind == LinkKind::Custom)
                targets.push_back(candidate.target);
        return targets;
    };
    using Targets = std::vector<std::string>;

    SECTION("no url: the match opens as printed")
    {
        auto const patterns = docs(std::nullopt);
        CHECK(linksOf(patterns, "smb://evil/x.pdf").empty());
        CHECK(linksOf(patterns, "ms-msdt:id/x.pdf").empty());
        CHECK(linksOf(patterns, "report.pdf").empty()); // no scheme at all: none a link may open
        CHECK(linksOf(patterns, "https://ok.example/x.pdf") == Targets { "https://ok.example/x.pdf" });
        CHECK(linksOf(patterns, "FTP://ok.example/x.pdf") == Targets { "FTP://ok.example/x.pdf" });
    }

    SECTION("a url that does not start with a scheme of its own")
    {
        auto const patterns = docs("{0}#page=1");
        CHECK(linksOf(patterns, "smb://evil/x.pdf").empty());
        CHECK(linksOf(patterns, "https://ok.example/x.pdf") == Targets { "https://ok.example/x.pdf#page=1" });
    }

    SECTION("a url with a literal scheme: any scheme, as the user wrote it")
    {
        auto const patterns = docs("myviewer://open?file={0}");
        CHECK(linksOf(patterns, "smb://evil/x.pdf") == Targets { "myviewer://open?file=smb://evil/x.pdf" });
    }

    SECTION("hint mode still offers what it may not open, to copy")
    {
        auto const found = findLinks(
            plain("smb://evil/x.pdf"), docs(std::nullopt), PatternSelection::All, LinkOverlap::Keep);
        CHECK(std::ranges::count(found, LinkKind::Custom, &LinkCandidate::kind) == 1);
    }
}

TEST_CASE("LinkMatcher.matchEndsOnAWideCharactersSecondColumn", "[links]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(40) } };
    // 東 takes columns 0-1 and the fullwidth full stop 。 columns 2-3 (blanked for matching, so the URL
    // starts right after it, at column 4). パス takes columns 16-19, and the URL ends on the second column of
    // ス with another blanked 。 behind it.
    mock.writeToScreen("東。https://x.y/パス。");
    auto rows = std::vector<ScanRow> { makeScanRow(
        mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)), LineOffset(0), LineContinuation::No) };
    auto const text = ScanText { rows, ScanClip {} };
    auto const found =
        findLinks(text, *LinkPatterns::builtins(), PatternSelection::ClickableOnly, LinkOverlap::Merge);
    auto const url = std::ranges::find(found, LinkKind::Url, &LinkCandidate::kind);
    REQUIRE(url != found.end());
    CHECK(url->target == "https://x.y/パス");
    CHECK(url->first == ScanPosition { .row = 0, .column = ColumnOffset(4) });
    // https://x.y/ is 12 columns (4..15), パス follows (16..19).
    CHECK(url->last == ScanPosition { .row = 0, .column = ColumnOffset(19) });
}

TEST_CASE("LinkMatcher.urlWrappedAcrossRows", "[links]")
{
    auto rows = std::vector<ScanRow> { makePlainScanRow("go https://x.y", LineOffset(0)),
                                       makePlainScanRow("/rest more", LineOffset(1), LineContinuation::Yes) };
    auto const text = ScanText { rows, ScanClip {} };
    auto const found =
        findLinks(text, *LinkPatterns::builtins(), PatternSelection::ClickableOnly, LinkOverlap::Merge);
    // The bare word "go" at byte 0 is a path candidate and survives the merge: find the URL by its kind.
    auto const url = std::ranges::find(found, LinkKind::Url, &LinkCandidate::kind);
    REQUIRE(url != found.end());
    CHECK(url->target == "https://x.y/rest");
    CHECK(url->first == ScanPosition { .row = 0, .column = ColumnOffset(3) });
    CHECK(url->last == ScanPosition { .row = 1, .column = ColumnOffset(4) });
}

TEST_CASE("LinkMatcher.locationThatOverflowsIsNoLocation", "[links]")
{
    auto const found = clickable("x.cpp:99999999999");
    REQUIRE_FALSE(found.empty());
    CHECK_FALSE(found[0].location.has_value());
}

TEST_CASE("LinkMatcher.columnThatOverflowsIsNoLocation", "[links]")
{
    // A printed column that does not fit is no location; it is not rewritten to column 1.
    auto const found = clickable("x.cpp:7:99999999999");
    REQUIRE_FALSE(found.empty());
    CHECK_FALSE(found[0].location.has_value());
    CHECK(found[0].target == "x.cpp");

    auto const lineOnly = clickable("x.cpp:7");
    REQUIRE_FALSE(lineOnly.empty());
    CHECK(lineOnly[0].location == LinkLocation { .line = 7, .column = 1 });
}

TEST_CASE("LinkMatcher.longLineIsMatchedWithoutExhaustingTheStack", "[links]")
{
    // A crash test, not a timing test (§11): every CI leg's std::regex must survive the budget's worth.
    auto const words = std::string(MaxScanCells, 'a');
    CHECK_FALSE(
        findLinks(plain(words), *LinkPatterns::builtins(), PatternSelection::All, LinkOverlap::Keep).empty());
    auto const url = "https://x.y/" + std::string(MaxScanCells - 12, 'b');
    CHECK(clickable(url).size() == 1);
}

TEST_CASE("LinkMatcher.trimmedLength", "[links]")
{
    CHECK(trimmedLength("a.") == 1);
    CHECK(trimmedLength("a/") == 2);
    CHECK(trimmedLength("a_(b)") == 5);
    CHECK(trimmedLength("a)") == 1);
    CHECK(trimmedLength("a=") == 2); // base64 padding is Sm, not punctuation
    CHECK(trimmedLength("a~") == 2);
    CHECK(trimmedLength("a\xEF\xBC\x81") == 1); // FULLWIDTH EXCLAMATION MARK
    CHECK(trimmedLength("a\xEF\xBC") == 3);     // a truncated sequence is not punctuation
}
