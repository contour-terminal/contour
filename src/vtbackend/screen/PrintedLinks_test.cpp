// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/links/PathProbe.hpp>
#include <vtbackend/screen/PrintedLinks.hpp>

#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

using namespace vtbackend;

namespace
{
/// Where the tests' files are. Absolute here: on Windows a root directory without a drive is relative to the
/// current drive.
#ifdef _WIN32
constexpr auto Work = std::string_view { "C:/w" };
#else
constexpr auto Work = std::string_view { "/w" };
#endif

/// @return A path link as printed: @p text, nothing looked up.
[[nodiscard]] LinkTarget printedPath(std::string text)
{
    return LinkTarget { .kind = LinkKind::Path,
                        .target = std::move(text),
                        .location = std::nullopt,
                        .pathKind = PathKind::Missing,
                        .directory = {} };
}

/// @return Bases holding only Work.
[[nodiscard]] std::expected<PathBases, PathRefusal> workBases()
{
    return PathBases { std::string { Work } };
}

/// @return @p path, normalised, as a probe or a resolved target spells it.
[[nodiscard]] std::filesystem::path normal(std::string_view path)
{
    return std::filesystem::path { path }.lexically_normal();
}
} // namespace

TEST_CASE("PrintedLinks.aUrlIsWhatItOpensWhereverItWasPrinted", "[links]")
{
    auto const url = LinkTarget { .kind = LinkKind::Url,
                                  .target = "https://x.y/",
                                  .location = std::nullopt,
                                  .pathKind = PathKind::Missing,
                                  .directory = {} };
    auto const classified =
        classifyPrintedLink(url, std::unexpected(PathRefusal::RanElsewhere), LinkMachine {}, nullptr);
    REQUIRE(std::holds_alternative<LinkTarget>(classified));
    CHECK(std::get<LinkTarget>(classified) == url);
}

TEST_CASE("PrintedLinks.aPathOnARowPrintedElsewhereIsRefused", "[links]")
{
    // Even an absolute one: it names a file on that machine.
    auto const classified = classifyPrintedLink(printedPath(std::string { Work } + "/a.txt"),
                                                std::unexpected(PathRefusal::ReplicaOfAnotherMachine),
                                                LinkMachine {},
                                                nullptr);
    REQUIRE(std::holds_alternative<RefusedPath>(classified));
    CHECK(std::get<RefusedPath>(classified).reason == PathRefusal::ReplicaOfAnotherMachine);
    CHECK(std::get<RefusedPath>(classified).printed.target == std::string { Work } + "/a.txt");
}

TEST_CASE("PrintedLinks.aRelativePathWithNowhereToLookIsMissing", "[links]")
{
    auto const classified = classifyPrintedLink(printedPath("a.txt"), PathBases {}, LinkMachine {}, nullptr);
    REQUIRE(std::holds_alternative<MissingPath>(classified));
    CHECK(std::get<MissingPath>(classified).printed.target == "a.txt");
}

TEST_CASE("PrintedLinks.anUnknownPathIsToBeLookedFor", "[links]")
{
    auto const classified = classifyPrintedLink(printedPath("a.txt"), workBases(), LinkMachine {}, nullptr);
    REQUIRE(std::holds_alternative<PathToProbe>(classified));
    auto const& toProbe = std::get<PathToProbe>(classified);
    REQUIRE(toProbe.query.candidates.size() == 1);
    CHECK(toProbe.query.candidates.front() == normal(std::string { Work } + "/a.txt"));
    CHECK(toProbe.directories == std::vector<std::string> { std::string { Work } });
    CHECK(toProbe.printed.target == "a.txt");
}

TEST_CASE("PrintedLinks.theProbesCacheAnswersWhereItKnows", "[links]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    files.addFile(normal(std::string { Work } + "/here.txt"), "");
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { files, clock, PathProbeLimits {} };
    for (auto const name: { "here.txt", "gone.txt" })
        std::ignore = probe.probe(PathQuery { .candidates = { normal(std::string { Work } + "/" + name) } });

    auto const found = classifyPrintedLink(printedPath("here.txt"), workBases(), LinkMachine {}, &probe);
    REQUIRE(std::holds_alternative<LinkTarget>(found));
    CHECK(std::get<LinkTarget>(found).pathKind == PathKind::File);
    CHECK(normal(std::get<LinkTarget>(found).target) == normal(std::string { Work } + "/here.txt"));
    CHECK(std::get<LinkTarget>(found).directory == Work);

    CHECK(std::holds_alternative<MissingPath>(
        classifyPrintedLink(printedPath("gone.txt"), workBases(), LinkMachine {}, &probe)));
    CHECK(std::holds_alternative<PathToProbe>(
        classifyPrintedLink(printedPath("new.txt"), workBases(), LinkMachine {}, &probe)));
}

namespace
{
/// @return A URL link as printed.
[[nodiscard]] LinkTarget printedUrl(std::string text)
{
    return LinkTarget { .kind = LinkKind::Url,
                        .target = std::move(text),
                        .location = std::nullopt,
                        .pathKind = PathKind::Missing,
                        .directory = {} };
}

/// @return @p path as a file:// URL with an empty authority, as POSIX spells one and Windows with a drive.
[[nodiscard]] std::string fileUrlOf(std::string_view path)
{
    return std::string { "file://" } + (path.starts_with('/') ? "" : "/") + std::string { path };
}
} // namespace

TEST_CASE("PrintedLinks.aFileUrlIsThePathItNames", "[links]")
{
    // §7.6: printed text never reaches the opener of OSC 8 links, which runs a program.
    auto const tool = std::string { Work } + "/bin/tool";
    auto const machine = LinkMachine { .home = {}, .hostname = "here" };

    SECTION("of this machine: a path to look for, refused where a path is")
    {
        for (auto const& url: { fileUrlOf(tool), "FILE://here" + fileUrlOf(tool).substr(7) })
        {
            auto const classified = classifyPrintedLink(printedUrl(url), PathBases {}, machine, nullptr);
            REQUIRE(std::holds_alternative<PathToProbe>(classified));
            auto const& toProbe = std::get<PathToProbe>(classified);
            CHECK(toProbe.printed.kind == LinkKind::Path);
            CHECK(normal(toProbe.printed.target) == normal(tool));
            REQUIRE(toProbe.query.candidates.size() == 1);
            CHECK(toProbe.query.candidates.front() == normal(tool));
        }
        auto const refused = classifyPrintedLink(
            printedUrl(fileUrlOf(tool)), std::unexpected(PathRefusal::RanElsewhere), machine, nullptr);
        REQUIRE(std::holds_alternative<RefusedPath>(refused));
        CHECK(std::get<RefusedPath>(refused).printed.kind == LinkKind::Path);
    }

    SECTION("of another machine: no link")
    {
        auto const elsewhere = "file://there" + fileUrlOf(tool).substr(7);
        CHECK(std::holds_alternative<NotALink>(
            classifyPrintedLink(printedUrl(elsewhere), PathBases {}, machine, nullptr)));
    }

    SECTION("a custom link expanding to one goes the same way")
    {
        auto custom = printedUrl(fileUrlOf(tool));
        custom.kind = LinkKind::Custom;
        CHECK(
            std::holds_alternative<PathToProbe>(classifyPrintedLink(custom, PathBases {}, machine, nullptr)));
    }
}

TEST_CASE("PrintedLinks.isFileUrl", "[links]")
{
    CHECK(isFileUrl("file:///x"));
    CHECK(isFileUrl("FILE://host/x"));
    CHECK_FALSE(isFileUrl("https://x"));
    CHECK_FALSE(isFileUrl("/x/file:y"));
    CHECK_FALSE(isFileUrl("files://x"));
}
