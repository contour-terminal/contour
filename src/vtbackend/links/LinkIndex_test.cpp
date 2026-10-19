// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkIndex.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace vtbackend;

namespace
{
DetectedLink link(std::string const& target, std::vector<LinkRowSpan> spans)
{
    return DetectedLink { .id = {},
                          .target = LinkTarget { .kind = LinkKind::Url,
                                                 .target = target,
                                                 .location = {},
                                                 .pathKind = PathKind::Missing,
                                                 .directory = {} },
                          .text = target,
                          .spans = std::move(spans) };
}

LinkRowSpan span(int64_t row, int first, int last)
{
    return LinkRowSpan { .row = row, .first = ColumnOffset(first), .last = ColumnOffset(last), .link = {} };
}

/// The target of @p found, or the empty string when nothing was found, so a miss fails a CHECK, not the run.
std::string targetOf(DetectedLink const* found)
{
    return found ? found->target.target : std::string {};
}
} // namespace

TEST_CASE("LinkIndex.publishAndLookUp", "[links]")
{
    auto index = LinkIndex {};
    auto const rows =
        std::array { ScannedRow { .id = 10, .revision = 3 }, ScannedRow { .id = 11, .revision = 4 } };
    index.publish(rows, { link("https://a.b/wrapped", { span(10, 5, 19), span(11, 0, 6) }) });

    REQUIRE(index.at(10, ColumnOffset(5)) != nullptr);
    CHECK(index.at(10, ColumnOffset(5)) == index.at(11, ColumnOffset(6))); // one link across a wrap
    CHECK(index.at(10, ColumnOffset(4)) == nullptr);
    CHECK(index.at(11, ColumnOffset(7)) == nullptr);
    CHECK(index.scannedRevision(10) == 3);
    CHECK(index.scannedRevision(12) == std::nullopt);
    CHECK(index.onRow(11).size() == 1);
}

TEST_CASE("LinkIndex.republishingALineReplacesIt", "[links]")
{
    auto index = LinkIndex {};
    auto const before = std::array { ScannedRow { .id = 1, .revision = 1 } };
    index.publish(before, { link("https://old/", { span(1, 0, 11) }) });
    auto const after = std::array { ScannedRow { .id = 1, .revision = 2 } };
    index.publish(after, {});
    CHECK(index.at(1, ColumnOffset(0)) == nullptr);
    CHECK(index.scannedRevision(1) == 2);
    CHECK(index.empty() == true);
}

TEST_CASE("LinkIndex.republishingOneRowForgetsTheWholeWrappedLink", "[links]")
{
    auto index = LinkIndex {};
    auto const rows =
        std::array { ScannedRow { .id = 1, .revision = 1 }, ScannedRow { .id = 2, .revision = 1 } };
    index.publish(rows, { link("https://wrapped/", { span(1, 5, 9), span(2, 0, 3) }) });
    REQUIRE(index.at(1, ColumnOffset(5)) != nullptr);
    auto const oldId = index.at(1, ColumnOffset(5))->id;

    auto const republished = std::array { ScannedRow { .id = 1, .revision = 2 } };
    index.publish(republished, {});

    CHECK(index.onRow(2).empty());
    CHECK(index.find(oldId) == nullptr);
    CHECK(index.at(2, ColumnOffset(0)) == nullptr);
}

TEST_CASE("LinkIndex.publishDropsALinkWithASpanOnAnUnlistedRow", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 1, .revision = 1 } };
    index.publish(rows, { link("p", { span(1, 0, 3), span(7, 0, 3) }) });
    CHECK(index.at(1, ColumnOffset(0)) == nullptr);
    CHECK(index.scannedRevision(7) == std::nullopt);
    CHECK(index.empty() == true);
}

TEST_CASE("LinkIndex.publishDropsALinkWithNoSpans", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 1, .revision = 1 } };
    index.publish(rows, { link("n", {}) });
    CHECK(index.empty() == true);
}

TEST_CASE("LinkIndex.addRefusesARowScannedAtAnotherRevision", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 5, .revision = 7 } };
    index.publish(rows, {});
    auto const stale = std::array { ScannedRow { .id = 5, .revision = 6 } };
    index.add(link("/tmp/x", { span(5, 0, 5) }), stale);
    CHECK(index.at(5, ColumnOffset(0)) == nullptr);
    index.add(link("/tmp/x", { span(5, 0, 5) }), rows);
    CHECK(index.at(5, ColumnOffset(0)) != nullptr);
}

TEST_CASE("LinkIndex.addRefusesARowThatWasNeverPublished", "[links]")
{
    auto index = LinkIndex {};
    auto const unknown = std::array { ScannedRow { .id = 99, .revision = 0 } };
    index.add(link("/tmp/y", { span(99, 0, 5) }), unknown);
    CHECK(index.at(99, ColumnOffset(0)) == nullptr);
    CHECK(index.empty() == true);
}

TEST_CASE("LinkIndex.addRefusesAnEmptyRowList", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 1, .revision = 1 } };
    index.publish(rows, {});
    index.add(link("/tmp/z", { span(1, 0, 3) }), std::span<ScannedRow const> {});
    CHECK(index.at(1, ColumnOffset(0)) == nullptr);
    CHECK(index.empty() == true);
}

TEST_CASE("LinkIndex.addRefusesASpanOnARowOutsideTheGivenRows", "[links]")
{
    auto index = LinkIndex {};
    auto const rows =
        std::array { ScannedRow { .id = 1, .revision = 1 }, ScannedRow { .id = 2, .revision = 1 } };
    index.publish(rows, {});
    auto const onlyRowOne = std::array { ScannedRow { .id = 1, .revision = 1 } };
    index.add(link("/tmp/w", { span(1, 0, 3), span(2, 0, 3) }), onlyRowOne);
    CHECK(index.at(1, ColumnOffset(0)) == nullptr);
    CHECK(index.at(2, ColumnOffset(0)) == nullptr);
    CHECK(index.empty() == true);
}

TEST_CASE("LinkIndex.nextAndPreviousWalkLinkStartsInReadingOrder", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 1, .revision = 1 },
                                   ScannedRow { .id = 2, .revision = 1 },
                                   ScannedRow { .id = 3, .revision = 1 } };
    index.publish(rows,
                  { link("a", { span(1, 2, 4) }),
                    link("b", { span(1, 8, 19), span(2, 0, 3) }),
                    link("c", { span(3, 0, 1) }) });

    CHECK(targetOf(index.next(1, ColumnOffset(0))) == "a");
    CHECK(targetOf(index.next(1, ColumnOffset(2))) == "b");
    CHECK(targetOf(index.next(1, ColumnOffset(8))) == "c"); // b's continuation on row 2 starts nothing
    CHECK(index.next(3, ColumnOffset(0)) == nullptr);
    CHECK(targetOf(index.previous(3, ColumnOffset(0))) == "b");
    CHECK(index.previous(1, ColumnOffset(2)) == nullptr);
}

TEST_CASE("LinkIndex.previousFromBetweenTwoLinksOnOneRow", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 1, .revision = 1 } };
    index.publish(rows, { link("a", { span(1, 2, 4) }), link("b", { span(1, 8, 19) }) });
    CHECK(targetOf(index.previous(1, ColumnOffset(6))) == "a");
    CHECK(targetOf(index.previous(1, ColumnOffset(8))) == "a");
    CHECK(targetOf(index.previous(1, ColumnOffset(9))) == "b");
}

TEST_CASE("LinkIndex.nextSkipsAContinuationSpanOnTheStartingRow", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 1, .revision = 1 },
                                   ScannedRow { .id = 2, .revision = 1 },
                                   ScannedRow { .id = 3, .revision = 1 } };
    index.publish(rows, { link("b", { span(1, 8, 19), span(2, 0, 3) }), link("c", { span(3, 0, 1) }) });
    CHECK(targetOf(index.next(2, ColumnOffset(0))) == "c");
}

TEST_CASE("LinkIndex.retainDropsRowsAndTheLinksTouchingThem", "[links]")
{
    auto index = LinkIndex {};
    auto const rows = std::array { ScannedRow { .id = 1, .revision = 1 },
                                   ScannedRow { .id = 2, .revision = 1 },
                                   ScannedRow { .id = 3, .revision = 1 } };
    index.publish(rows, { link("w", { span(1, 5, 9), span(2, 0, 3) }), link("k", { span(3, 0, 4) }) });
    REQUIRE(index.at(1, ColumnOffset(5)) != nullptr);
    auto const wId = index.at(1, ColumnOffset(5))->id;

    index.retain(2, 10);

    CHECK(index.scannedRevision(1) == std::nullopt);
    CHECK(index.onRow(2).empty());
    CHECK(index.find(wId) == nullptr);
    CHECK(index.at(2, ColumnOffset(0)) == nullptr);
    CHECK(targetOf(index.at(3, ColumnOffset(0))) == "k"); // a link wholly on a kept row survives
    CHECK(index.empty() == false);

    index.retain(10, 20);
    CHECK(index.empty() == true);
}
