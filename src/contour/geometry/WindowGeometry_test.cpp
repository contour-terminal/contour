// SPDX-License-Identifier: Apache-2.0
//
// Table-driven tests for the pure window-geometry module. The highest-value property here is the
// anti-oscillation roundtrip invariant: a window sized FOR a grid must fit EXACTLY that grid
// (pageSizeForPixels(requiredPixelsForPage(p)) == p) at every cell size, margin and content scale.
// The historic sizing bugs were divergent conversions violating exactly this.

#include <contour/geometry/WindowGeometry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <ranges>

using namespace contour::geometry;

using vtbackend::ColumnCount;
using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::LineCount;
using vtbackend::PageSize;
using vtbackend::Width;

namespace
{

constexpr ImageSize imageSize(int width, int height) noexcept
{
    return { .width = Width::cast_from(width), .height = Height::cast_from(height) };
}

constexpr PageSize pageSize(int lines, int columns) noexcept
{
    return { .lines = LineCount(lines), .columns = ColumnCount(columns) };
}

constexpr auto IdentityClamp = [](PageSize p) noexcept {
    return p;
};

/// Mirrors Terminal::clampedTotalPageSize for a given status-line height (Terminal.h): the total page must
/// leave room for at least one main-page line on top of the status line(s), and at least one column.
constexpr auto statusLineClamp(int statusLineHeight) noexcept
{
    return [statusLineHeight](PageSize p) noexcept {
        return PageSize { .lines = std::max(p.lines, LineCount(statusLineHeight + 1)),
                          .columns = std::max(p.columns, ColumnCount(1)) };
    };
}

// Compile-time proof that the core conversions are constexpr-usable.
static_assert(pageSizeForPixels(imageSize(800, 600), imageSize(10, 20), Margins {}) == pageSize(30, 80));
static_assert(requiredPixelsForPage(pageSize(30, 80), imageSize(10, 20), Margins {}) == imageSize(800, 600));
static_assert(resolveContentScale(144.0, 2.0, std::nullopt) == 1.5);

// What applications are told a cell is. An application recovers the cell by dividing a reported extent
// by the grid (`ws_ypixel / ws_row`), so a report is only usable where that division is exact.
static_assert(reportedCellSize(imageSize(10, 20), 1.0) == imageSize(10, 20));
static_assert(reportedCellSize(imageSize(20, 40), 2.0) == imageSize(10, 20));
static_assert(reportedCellSize(imageSize(19, 44), 1.75) == imageSize(10, 25));
// Scale 1.0 -- PixelReporting::Device -- reports the renderer's own cell untouched. Since that cell IS
// the font's advance in device pixels, this is the one setting that round-trips for every font.
static_assert(reportedCellSize(imageSize(19, 44), 1.0) == imageSize(19, 44));
static_assert(reportedCellSize(imageSize(17, 39), 1.0) == imageSize(17, 39));
// FLOOR, not ceil: a report of available space must never promise more than exists.
static_assert(reportedCellSize(imageSize(19, 44), 2.0) == imageSize(9, 22));
// A cell never floors to nothing, or the report could not be divided back into one.
static_assert(reportedCellSize(imageSize(1, 1), 4.0) == imageSize(1, 1));
// A degenerate scale reports what the renderer uses rather than collapsing.
static_assert(reportedCellSize(imageSize(10, 20), 0.0) == imageSize(10, 20));

/// Whether reporting @p cell under @p scale keeps the cell's ASPECT RATIO, compared exactly by
/// cross-multiplication rather than by dividing.
///
/// This is the property that decides whether a full-screen image has a gap. An application sizes its
/// canvas from the REPORTED cell, but RasterizedImage::fragmentPlacement aspect-fits that canvas
/// (ImageResize::ResizeToFit) into the grid measured in DEVICE cells. When the two aspects disagree,
/// ResizeToFit's std::min() honors whichever axis lost less to the floor and letterboxes the other.
/// @param cell  Cell size in device pixels, as the renderer works in.
/// @param scale Device pixels per logical pixel to divide out; 1.0 reports device pixels as-is.
/// @return Whether the reported cell is similar to @p cell.
[[nodiscard]] constexpr bool preservesAspect(ImageSize cell, double scale) noexcept
{
    auto const reported = reportedCellSize(cell, scale);
    return unbox<int>(cell.width) * unbox<int>(reported.height)
           == unbox<int>(cell.height) * unbox<int>(reported.width);
}

// Device reporting is exact, so the aspect survives for EVERY cell: the fit scale is 1.0 and the image
// lands 1:1, gapless on both axes. This is why Device is the default.
static_assert(preservesAspect(imageSize(17, 39), 1.0));
static_assert(preservesAspect(imageSize(19, 44), 1.0));
static_assert(preservesAspect(imageSize(11, 25), 1.0));
// Logical reporting survives only where the scale divides both axes evenly.
static_assert(preservesAspect(imageSize(20, 40), 2.0));
// ... and at a fractional scale it does not, which IS the ~6% gap down the right of a full-screen
// sixel: 17/1.75 = 9.714 -> 9 loses 7.4% of the width, but 39/1.75 = 22.29 -> 22 loses only 1.3% of
// the height, so min() fills the height and letterboxes the width. No rounding mode fixes this --
// only reporting the unit the cell is an integer in does.
static_assert(!preservesAspect(imageSize(17, 39), 1.75));
static_assert(!preservesAspect(imageSize(19, 44), 1.75));

// The report divides back to exactly the cell it was built from -- which is the whole contract, since
// Terminal::resizeScreen recovers the cell size by dividing this by the page.
static_assert(reportedPixelsForPage(pageSize(30, 80), imageSize(10, 20), 1.0) == imageSize(800, 600));
static_assert(reportedPixelsForPage(pageSize(30, 80), imageSize(20, 40), 2.0) == imageSize(800, 600));

// Compile-time proof of window/pane-preserving font-zoom monotonicity: at FIXED pixels, a larger cell
// (bigger font) yields fewer columns/lines than the base, and a smaller cell yields more. See the
// runtime case "WindowGeometry.pageSizeForPixels.cellSizeMonotonicity".
static_assert(pageSizeForPixels(imageSize(800, 600), imageSize(14, 30), Margins {}) == pageSize(20, 57));
static_assert(pageSizeForPixels(imageSize(800, 600), imageSize(8, 16), Margins {}) == pageSize(37, 100));

// Compile-time proof of the spawn-context rule (see initialPageSize / the "new tab adopts the running
// window size" fix): a running size wins over the profile default; only its absence falls back.
static_assert(initialPageSize(pageSize(40, 200), pageSize(25, 80)) == pageSize(40, 200));
static_assert(initialPageSize(std::nullopt, pageSize(25, 80)) == pageSize(25, 80));

} // namespace

TEST_CASE("WindowGeometry.pageSizeForPixels.floorSemantics", "[contour][geometry]")
{
    struct Row
    {
        ImageSize available;
        ImageSize cell;
        Margins margins;
        PageSize expected;
    };
    auto const row = GENERATE(Row { imageSize(800, 600), imageSize(10, 20), Margins {}, pageSize(30, 80) },
                              // One pixel short of the boundary floors down; one past keeps the grid:
                              Row { imageSize(799, 599), imageSize(10, 20), Margins {}, pageSize(29, 79) },
                              Row { imageSize(801, 601), imageSize(10, 20), Margins {}, pageSize(30, 80) },
                              // Non-divisible cell sizes:
                              Row { imageSize(100, 100), imageSize(7, 15), Margins {}, pageSize(6, 14) },
                              // Margins are applied on BOTH sides of each axis:
                              Row { imageSize(809, 639),
                                    imageSize(10, 20),
                                    Margins { .horizontal = 4, .vertical = 2 },
                                    pageSize(31, 80) });

    CAPTURE(row.available, row.cell, row.margins.horizontal, row.margins.vertical);
    CHECK(pageSizeForPixels(row.available, row.cell, row.margins) == row.expected);
}

TEST_CASE("WindowGeometry.pageSizeForPixels.fractionalScale", "[contour][geometry]")
{
    // Logical item sizes floored to device pixels at fractional scales, then floored to the grid — the
    // window->grid path exactly as the display drives it, with realistic FreeType cell sizes.
    struct Row
    {
        double logicalWidth;
        double logicalHeight;
        double scale;
        ImageSize cell;
        PageSize expected;
    };
    auto const row = GENERATE(Row { 640, 480, 1.25, imageSize(9, 19), pageSize(31, 88) },
                              Row { 640, 480, 1.5, imageSize(11, 23), pageSize(31, 87) },
                              Row { 640, 480, 2.0, imageSize(14, 30), pageSize(32, 91) });

    CAPTURE(row.scale, row.cell);
    auto const device = availableDevicePixels(row.logicalWidth, row.logicalHeight, row.scale);
    CHECK(pageSizeForPixels(device, row.cell, Margins {}) == row.expected);
}

TEST_CASE("WindowGeometry.pageSizeForPixels.degenerateInputsClampInsteadOfWrapping", "[contour][geometry]")
{
    auto const cell = imageSize(10, 20);

    // Margins exceed the area: usable is clamped to 0, page to 1x1 — the unsigned-underflow class the
    // old helper::pageSizeForPixels wrapped on (UBSan is blind to unsigned wrap; only this test catches it).
    CHECK(pageSizeForPixels(imageSize(10, 10), cell, Margins { .horizontal = 20, .vertical = 20 })
          == pageSize(1, 1));

    // Margins equal the area:
    CHECK(pageSizeForPixels(imageSize(40, 40), cell, Margins { .horizontal = 20, .vertical = 20 })
          == pageSize(1, 1));

    // Zero-sized area:
    CHECK(pageSizeForPixels(imageSize(0, 0), cell, Margins {}) == pageSize(1, 1));

    // Zero cell size: defined (no division by zero), clamps the divisor to 1.
    CHECK(pageSizeForPixels(imageSize(30, 40), imageSize(0, 0), Margins {}) == pageSize(40, 30));
}

TEST_CASE("WindowGeometry.pageSizeForPixels.cellSizeMonotonicity", "[contour][geometry]")
{
    // The mathematical heart of window/pane-preserving font zoom: at a FIXED pixel extent, a larger cell
    // (bigger font) never yields a larger page and strictly shrinks it once a whole cell no longer fits;
    // a smaller cell (smaller font) never yields a smaller page. TerminalDisplay::setFontSize relies on
    // this — it reflows the grid against the pane's fixed pixels instead of resizing the window, so the
    // columns/lines (and the child PTY size) must move opposite to the font size.
    auto const available = imageSize(800, 600);

    auto const base = pageSizeForPixels(available, imageSize(10, 20), Margins {});
    CHECK(base == pageSize(30, 80));

    // Bigger font (larger cell) -> fewer columns and rows.
    auto const bigger = pageSizeForPixels(available, imageSize(14, 30), Margins {});
    CHECK(bigger == pageSize(20, 57));
    CHECK(bigger.lines <= base.lines);
    CHECK(bigger.columns <= base.columns);
    CHECK((bigger.lines < base.lines || bigger.columns < base.columns)); // strictly smaller somewhere

    // Smaller font (smaller cell) -> more columns and rows.
    auto const smaller = pageSizeForPixels(available, imageSize(8, 16), Margins {});
    CHECK(smaller == pageSize(37, 100));
    CHECK(smaller.lines >= base.lines);
    CHECK(smaller.columns >= base.columns);
    CHECK((smaller.lines > base.lines || smaller.columns > base.columns)); // strictly larger somewhere
}

TEST_CASE("WindowGeometry.reportedPixelsForPage.dividesBackToTheCell", "[contour][geometry]")
{
    // Terminal::resizeScreen recovers the cell size as `pixels / totalPageSize`, and applications
    // then size an image canvas from it. So the ONE property this report must have is that the
    // division comes back exact -- anything else is reported as cell-size error and multiplies out
    // into a wrongly-sized image. Swept across the scales a real display produces.
    auto const scale = GENERATE(1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0);
    auto const cellW = GENERATE(6, 8, 10, 13, 19, 25);
    auto const cellH = GENERATE(12, 17, 21, 25, 44);
    auto const cols = GENERATE(1, 80, 143, 240);
    auto const lines = GENERATE(1, 24, 62, 63);
    CAPTURE(scale, cellW, cellH, cols, lines);

    auto const page = pageSize(lines, cols);
    auto const reported = reportedPixelsForPage(page, imageSize(cellW, cellH), scale);
    auto const expectedCell = reportedCellSize(imageSize(cellW, cellH), scale);

    // This is the division Terminal::resizeScreen performs.
    CHECK(reported / page == expectedCell);
}

TEST_CASE("WindowGeometry.reportedPixelsForPage.excludesMargins", "[contour][geometry]")
{
    // requiredPixelsForPage() adds margins because it sizes a WINDOW. Telling an application that
    // same number makes the margins read as extra cell size, because resizeScreen() recovers the
    // cell by dividing by the page -- which is what attachDisplay() did.
    auto const page = pageSize(24, 80);
    auto const cell = imageSize(10, 20);

    SECTION("the report is cells and nothing else")
    {
        CHECK(reportedPixelsForPage(page, cell, 1.0) == imageSize(800, 480));
        CHECK(reportedPixelsForPage(page, cell, 1.0) / page == cell);
    }

    SECTION("small margins are absorbed by the division, which is why this went unnoticed")
    {
        // 814/80 == 10 and 490/24 == 20: the remainder is smaller than one cell, so truncation eats
        // it. This is the common case and it is why passing a window size here looked harmless.
        auto const margins = Margins { .horizontal = 7, .vertical = 5 };
        CHECK(requiredPixelsForPage(page, cell, margins) == imageSize(814, 490));
        CHECK(requiredPixelsForPage(page, cell, margins) / page == cell);
    }

    SECTION("a margin worth a whole cell row is not absorbed")
    {
        // Once 2*margin reaches the axis's cell count the remainder is a whole cell and the reported
        // cell grows. Lines are the exposed axis -- there are far fewer of them than columns -- so a
        // 12px vertical margin on 24 lines is enough: 20*24 + 24 = 504, and 504/24 == 21.
        auto const margins = Margins { .horizontal = 0, .vertical = 12 };
        CHECK(requiredPixelsForPage(page, cell, margins) == imageSize(800, 504));
        CHECK(requiredPixelsForPage(page, cell, margins) / page != cell);
        // Which is the whole point of reporting from the cell rather than from the window.
        CHECK(reportedPixelsForPage(page, cell, 1.0) / page == cell);
    }
}

TEST_CASE("WindowGeometry.fitPageToPixels.clampContract", "[contour][geometry]")
{
    // Restates the Terminal::clampedTotalPageSize contract (Terminal_test "resize.clamped.minimums") through
    // the injected clamp, so the frontend fit and the backend clamp can never drift apart.
    auto const cell = imageSize(10, 20);

    // Degenerate area, one status line: at least 1 main-page line + 1 status line, 1 column.
    CHECK(fitPageToPixels(imageSize(0, 0), cell, Margins {}, statusLineClamp(1)).pageSize == pageSize(2, 1));

    // Wide but one-line-high area, one status line:
    CHECK(fitPageToPixels(imageSize(800, 20), cell, Margins {}, statusLineClamp(1)).pageSize
          == pageSize(2, 80));

    // No status line: plain 1x1 minimum.
    CHECK(fitPageToPixels(imageSize(0, 0), cell, Margins {}, statusLineClamp(0)).pageSize == pageSize(1, 1));

    // Two status lines: 3-line minimum.
    CHECK(fitPageToPixels(imageSize(0, 0), cell, Margins {}, statusLineClamp(2)).pageSize == pageSize(3, 1));
}

TEST_CASE("WindowGeometry.fitPageToPixels.marginPlacement", "[contour][geometry]")
{
    auto const cell = imageSize(10, 20);
    auto const margins = Margins { .horizontal = 4, .vertical = 2 };

    SECTION("leftover >= configured margin: bottom capped at the configured margin")
    {
        // 609 - 4 = 605 usable -> 30 lines (600 used); leftover = 609 - 600 - 2 = 7 -> bottom = 2.
        auto const fit = fitPageToPixels(imageSize(800, 609), cell, margins, IdentityClamp);
        CHECK(fit.pageSize == pageSize(30, 79));
        CHECK(fit.pageMargin.left == 4);
        CHECK(fit.pageMargin.top == 2);
        CHECK(fit.pageMargin.bottom == 2);
    }

    SECTION("clamped page overflows the area: bottom clamps to 0 instead of wrapping")
    {
        // The clamp forces 2 lines (40px) into a 10px-high area; leftover is negative.
        auto const fit = fitPageToPixels(imageSize(10, 10), cell, margins, statusLineClamp(1));
        CHECK(fit.pageSize == pageSize(2, 1));
        CHECK(fit.pageMargin.bottom == 0);
    }
}

TEST_CASE("WindowGeometry.roundtrip.pageThroughPixels", "[contour][geometry]")
{
    // THE anti-oscillation invariant: a pixel area computed FOR a page yields exactly that page back.
    auto const cell = GENERATE(imageSize(5, 10),
                               imageSize(7, 15),
                               imageSize(8, 16),
                               imageSize(9, 19),
                               imageSize(10, 20),
                               imageSize(11, 23),
                               imageSize(14, 30),
                               imageSize(21, 42));
    auto const margin = GENERATE(0, 2, 10);
    auto const page = GENERATE(pageSize(1, 1),
                               pageSize(2, 2),
                               pageSize(25, 80),
                               pageSize(26, 80),
                               pageSize(50, 132),
                               pageSize(2, 500),
                               pageSize(500, 2));

    auto const margins = Margins { .horizontal = margin, .vertical = margin };
    CAPTURE(cell, margin, page.lines.value, page.columns.value);

    auto const required = requiredPixelsForPage(page, cell, margins);
    CHECK(pageSizeForPixels(required, cell, margins) == page);

    // Surplus property: any area at least as large fits at least that page, and the required area never
    // exceeds the given one by a full cell (the sub-cell remainder is strictly smaller than a cell).
    auto const surplus = imageSize(unbox<int>(required.width) + unbox<int>(cell.width) - 1,
                                   unbox<int>(required.height) + unbox<int>(cell.height) - 1);
    CHECK(pageSizeForPixels(surplus, cell, margins) == page);
}

TEST_CASE("WindowGeometry.roundtrip.pageThroughPixelsWithAGutter", "[contour][geometry]")
{
    // THE anti-oscillation invariant again, this time with a gutter reserved. It holds for the same
    // reason it holds for the margins: the gutter is an exact integer subtracted on the availability
    // side and added on the requirement side, so it cancels. A gutter that broke this would make a
    // window resize oscillate by a column for as long as the user held the mouse down.
    auto const cell = GENERATE(imageSize(5, 10), imageSize(8, 16), imageSize(11, 23), imageSize(21, 42));
    auto const margin = GENERATE(0, 2, 10);
    auto const gutter = GENERATE(0, 1, 8, 21, 100);
    auto const page = GENERATE(pageSize(1, 1), pageSize(25, 80), pageSize(50, 132), pageSize(2, 500));

    auto const margins = Margins { .horizontal = margin, .vertical = margin };
    CAPTURE(cell, margin, gutter, page.lines.value, page.columns.value);

    auto const required = requiredPixelsForPage(page, cell, margins, gutter);
    CHECK(pageSizeForPixels(required, cell, margins, gutter) == page);

    auto const surplus = imageSize(unbox<int>(required.width) + unbox<int>(cell.width) - 1,
                                   unbox<int>(required.height) + unbox<int>(cell.height) - 1);
    CHECK(pageSizeForPixels(surplus, cell, margins, gutter) == page);
}

TEST_CASE("WindowGeometry.gutter.costsExactlyItsOwnWidth", "[contour][geometry]")
{
    // A gutter is width taken from the cells, and nothing else: the same window fits one fewer column
    // per cell-width of gutter, and the height is untouched.
    auto const cell = imageSize(10, 20);
    auto const margins = Margins { .horizontal = 5, .vertical = 5 };
    auto const available = imageSize(1000, 600);

    auto const without = pageSizeForPixels(available, cell, margins);
    auto const withGutter = pageSizeForPixels(available, cell, margins, /*gutterDevicePx*/ 10);

    CHECK(without.columns == vtbackend::ColumnCount(99));
    CHECK(withGutter.columns == vtbackend::ColumnCount(98));
    CHECK(withGutter.lines == without.lines);
}

TEST_CASE("WindowGeometry.gutter.landsInsideThePageMarginLeft", "[contour][geometry]")
{
    // The load-bearing placement: the gutter is folded into pageMargin.left, which is already the single
    // left inset every pixel<->cell conversion in the tree reads -- GridMetrics::map, the mouse
    // hit-test, the accessibility bridge. Folding it in there is what makes all of them correct at once,
    // and leaves [horizontal, horizontal + gutter) as the strip the renderer owns.
    auto const cell = imageSize(10, 20);
    auto const margins = Margins { .horizontal = 5, .vertical = 5 };
    auto const identity = [](vtbackend::PageSize page) {
        return page;
    };

    auto const fit = fitPageToPixels(imageSize(1000, 600), cell, margins, identity, /*gutterDevicePx*/ 10);

    CHECK(fit.pageMargin.left == 15);
    // Top and bottom are the gutter's business not at all.
    CHECK(fit.pageMargin.top == 5);
}

TEST_CASE("WindowGeometry.gutter.widensTheWindowAndItsSizeHintBase", "[contour][geometry]")
{
    // A window sized for a page must grow by the gutter, or the grid it was sized for would not fit --
    // and the WM's base size must grow with it, since the base is the fixed non-grid part.
    auto const cell = imageSize(10, 20);
    auto const marginsLogical = Margins { .horizontal = 5, .vertical = 5 };
    auto const page = pageSize(25, 80);
    auto const chrome = Chrome { .width = 0, .height = 30 };

    auto const without = windowSizeForPage(page, cell, marginsLogical, 1.0, chrome);
    auto const withGutter = windowSizeForPage(page, cell, marginsLogical, 1.0, chrome, 10);
    CHECK(withGutter.width == without.width + 10);
    CHECK(withGutter.height == without.height);

    auto const hintsWithout = sizeHintsFor(cell, marginsLogical, 1.0, chrome);
    auto const hintsWith = sizeHintsFor(cell, marginsLogical, 1.0, chrome, 10);
    CHECK(hintsWith.base.width == hintsWithout.base.width + 10);
    CHECK(hintsWith.base.height == hintsWithout.base.height);
    // One cell per column is unchanged -- a gutter is a fixed inset, not part of the resize grid.
    CHECK(hintsWith.increment == hintsWithout.increment);
    CHECK(hintsWith.minimum.width == hintsWithout.minimum.width + 10);
}

TEST_CASE("WindowGeometry.mainPageRowAt.maps pixels onto main-page rows", "[contour][geometry]")
{
    // Grid origin at y=10, 20px cells, no status line: screen rows and main-page rows coincide.
    auto constexpr MarginTop = 10;
    auto constexpr CellHeight = 20;
    auto constexpr Lines = 5;

    CHECK(mainPageRowAt(10, MarginTop, CellHeight, /*mainPageTopRow*/ 0, Lines) == 0);
    CHECK(mainPageRowAt(29, MarginTop, CellHeight, 0, Lines) == 0);
    CHECK(mainPageRowAt(30, MarginTop, CellHeight, 0, Lines) == 1);
    CHECK(mainPageRowAt(99, MarginTop, CellHeight, 0, Lines) == 4);

    // Off the grid, above and below: not a row, rather than the nearest one.
    CHECK(mainPageRowAt(9, MarginTop, CellHeight, 0, Lines) == std::nullopt);
    CHECK(mainPageRowAt(-100, MarginTop, CellHeight, 0, Lines) == std::nullopt);
    CHECK(mainPageRowAt(110, MarginTop, CellHeight, 0, Lines) == std::nullopt);
}

TEST_CASE("WindowGeometry.mainPageRowNear.clamps where its sibling declines", "[contour][geometry]")
{
    // The two differ ONLY in what they do off the page: a drag that leaves the grid must keep naming
    // its nearest row, because auto-scroll depends on that. Where mainPageRowAt() answers, the two
    // agree -- which is the property that keeps the cell hit-test and the gutter hit-test in step.
    auto constexpr MarginTop = 10;
    auto constexpr CellHeight = 20;
    auto constexpr Lines = 5;

    for (auto const y: { 10, 29, 30, 99, 109 })
        CHECK(mainPageRowNear(y, MarginTop, CellHeight, 0, Lines)
              == mainPageRowAt(y, MarginTop, CellHeight, 0, Lines));

    // Above the grid -- including the row immediately above it, which a truncating division would
    // have folded onto row 0 without ever leaving the page.
    CHECK(mainPageRowNear(9, MarginTop, CellHeight, 0, Lines) == 0);
    CHECK(mainPageRowNear(-100, MarginTop, CellHeight, 0, Lines) == 0);

    // Below it.
    CHECK(mainPageRowNear(110, MarginTop, CellHeight, 0, Lines) == 4);
    CHECK(mainPageRowNear(10'000, MarginTop, CellHeight, 0, Lines) == 4);

    // A status line above the grid shifts it, exactly as it shifts the declining sibling.
    CHECK(mainPageRowNear(0, MarginTop, CellHeight, /*mainPageTopRow*/ 1, Lines) == 0);
    CHECK(mainPageRowNear(30, MarginTop, CellHeight, 1, Lines) == 0);

    // Degenerate inputs answer with the only row that is always safe.
    CHECK(mainPageRowNear(100, MarginTop, /*cellHeightPx*/ 0, 0, Lines) == 0);
    CHECK(mainPageRowNear(100, MarginTop, CellHeight, 0, /*mainPageLines*/ 0) == 0);
}

TEST_CASE("WindowGeometry.pageMarginFor.folds the gutter into the left inset", "[contour][geometry]")
{
    // The property every pixel<->cell conversion in the tree depends on: the gutter lands INSIDE
    // pageMargin.left, so the grid starts past it and isInGutter() names exactly the strip it gained.
    auto constexpr Margins = contour::geometry::Margins { .horizontal = 6, .vertical = 4 };

    auto const bare = pageMarginFor(Margins);
    CHECK(bare.left == 6);
    CHECK(bare.top == 4);
    CHECK(bare.bottom == 4); // a page not yet fitted to a surface has the configured margin below it

    auto const gutter = pageMarginFor(Margins, /*gutterDevicePx*/ 9);
    CHECK(gutter.left == 15);
    CHECK(gutter.top == 4);
    CHECK(!isInGutter(5, gutter.left, 9));  // the configured margin, left of the gutter
    CHECK(isInGutter(6, gutter.left, 9));   // the gutter's first pixel
    CHECK(isInGutter(14, gutter.left, 9));  // its last
    CHECK(!isInGutter(15, gutter.left, 9)); // column 0

    // The fit supplies a measured bottom; everything else is the same placement.
    auto const fitted = pageMarginFor(Margins, 9, /*bottomDevicePx*/ 1);
    CHECK(fitted.left == 15);
    CHECK(fitted.bottom == 1);
}

TEST_CASE("WindowGeometry.mainPageRowAt.a top status line shifts the grid down", "[contour][geometry]")
{
    // THE regression this exists for. With a one-line status display at the TOP, the renderer draws
    // it on screen row 0 and the grid starts on row 1 -- so the pixels of the first GRID row belong
    // to main-page row 0, not row 1. Feeding a raw screen row to Viewport's translation shifted every
    // hit-test down by the status line's height, and dropped the grid's last row entirely.
    auto constexpr MarginTop = 0;
    auto constexpr CellHeight = 20;
    auto constexpr TopRow = 1;
    auto constexpr Lines = 5;

    // The status line itself is not part of the grid.
    CHECK(mainPageRowAt(0, MarginTop, CellHeight, TopRow, Lines) == std::nullopt);
    CHECK(mainPageRowAt(19, MarginTop, CellHeight, TopRow, Lines) == std::nullopt);

    // The first row BELOW it is main-page row 0.
    CHECK(mainPageRowAt(20, MarginTop, CellHeight, TopRow, Lines) == 0);
    CHECK(mainPageRowAt(39, MarginTop, CellHeight, TopRow, Lines) == 0);

    // ...and the last main-page row is reachable, which it was not while the shift was missing.
    CHECK(mainPageRowAt(100, MarginTop, CellHeight, TopRow, Lines) == 4);
    CHECK(mainPageRowAt(119, MarginTop, CellHeight, TopRow, Lines) == 4);
    CHECK(mainPageRowAt(120, MarginTop, CellHeight, TopRow, Lines) == std::nullopt);
}

TEST_CASE("WindowGeometry.mainPageRowAt.a bottom status line is not the grid", "[contour][geometry]")
{
    // A status line BELOW the grid leaves mainPageTopRow at zero; the rows past the page are simply
    // not on it, which is what keeps a click on the status line from acting on the last grid row.
    CHECK(mainPageRowAt(0, 0, 20, /*mainPageTopRow*/ 0, /*mainPageLines*/ 3) == 0);
    CHECK(mainPageRowAt(59, 0, 20, 0, 3) == 2);
    CHECK(mainPageRowAt(60, 0, 20, 0, 3) == std::nullopt);
}

TEST_CASE("WindowGeometry.mainPageRowAt.degenerate metrics answer nothing", "[contour][geometry]")
{
    // Before the first resize the renderer can hand out a zero cell height; dividing by it would be
    // undefined rather than merely wrong.
    CHECK(mainPageRowAt(50, 0, /*cellHeightPx*/ 0, 0, 5) == std::nullopt);
    CHECK(mainPageRowAt(50, 0, 20, 0, /*mainPageLines*/ 0) == std::nullopt);
}

TEST_CASE("WindowGeometry.isInGutter.covers the strip and nothing else", "[contour][geometry]")
{
    // Margin 5, gutter 10, so pageMargin.left is 15 and the strip is [5, 15). Column 0 starts at 15.
    auto constexpr MarginLeft = 5;
    auto constexpr Gutter = 10;
    auto constexpr PageMarginLeft = MarginLeft + Gutter;

    // The margin to the left of the gutter is not the gutter.
    CHECK(!isInGutter(0, PageMarginLeft, Gutter));
    CHECK(!isInGutter(4, PageMarginLeft, Gutter));

    // The strip itself, both edges.
    CHECK(isInGutter(5, PageMarginLeft, Gutter));
    CHECK(isInGutter(14, PageMarginLeft, Gutter));

    // The first cell column is past it -- half-open, so 15 is already the grid.
    CHECK(!isInGutter(15, PageMarginLeft, Gutter));
    CHECK(!isInGutter(500, PageMarginLeft, Gutter));
}

TEST_CASE("WindowGeometry.isInGutter.no gutter means nothing is ever in it", "[contour][geometry]")
{
    // With markers off there is no strip, and pageMargin.left is the plain margin: every x must fall
    // through to the ordinary cell hit-test, including the margin the grid does not start at.
    for (auto const x: { 0, 1, 4, 5, 100 })
    {
        CAPTURE(x);
        CHECK(!isInGutter(x, /*pageMarginLeft*/ 5, /*gutterDevicePx*/ 0));
    }
}

TEST_CASE("WindowGeometry.gutter.zeroIsExactlyTheUngutteredArithmetic", "[contour][geometry]")
{
    // The default, and what every existing caller passes: a zero gutter must leave every function
    // byte-for-byte what it was before one existed.
    auto const cell = imageSize(11, 23);
    auto const margins = Margins { .horizontal = 7, .vertical = 3 };
    auto const available = imageSize(999, 601);
    auto const page = pageSize(25, 80);
    auto const chrome = Chrome { .width = 4, .height = 30 };
    auto const identity = [](vtbackend::PageSize p) {
        return p;
    };

    CHECK(pageSizeForPixels(available, cell, margins, 0) == pageSizeForPixels(available, cell, margins));
    CHECK(requiredPixelsForPage(page, cell, margins, 0) == requiredPixelsForPage(page, cell, margins));
    CHECK(fitPageToPixels(available, cell, margins, identity, 0).pageMargin.left
          == fitPageToPixels(available, cell, margins, identity).pageMargin.left);
    CHECK(windowSizeForPage(page, cell, margins, 1.25, chrome, 0)
          == windowSizeForPage(page, cell, margins, 1.25, chrome));
    CHECK(sizeHintsFor(cell, margins, 1.25, chrome, 0) == sizeHintsFor(cell, margins, 1.25, chrome));
}

TEST_CASE("WindowGeometry.roundtrip.pageThroughWindowAtScale", "[contour][geometry]")
{
    // The full grid->window->grid cycle at every scale: a window sized for a page, converted back through
    // the floor-availability path, recovers exactly that page. This turns the prose "floor availability,
    // ceil requirement" law into an executable test across fractional scales (incl. 4/3, which does not
    // have an exact binary representation).
    auto const scale = GENERATE(1.0, 1.25, 1.5, 1.75, 2.0, 4.0 / 3.0);
    auto const cell = GENERATE(imageSize(9, 19), imageSize(10, 20), imageSize(11, 23));
    auto const page = GENERATE(pageSize(25, 80), pageSize(51, 121));
    auto const chrome = Chrome { .width = 0, .height = 34 };
    auto const marginsDevice = Margins { .horizontal = 4, .vertical = 4 };

    CAPTURE(scale, cell, page.lines.value, page.columns.value);

    auto const window = windowSizeForPage(page, cell, marginsDevice, scale, chrome);
    auto const contentLogicalWidth = static_cast<double>(window.width - chrome.width);
    auto const contentLogicalHeight = static_cast<double>(window.height - chrome.height);
    auto const device = availableDevicePixels(contentLogicalWidth, contentLogicalHeight, scale);

    CHECK(pageSizeForPixels(device, cell, marginsDevice) == page);
}

TEST_CASE("WindowGeometry.windowSizeForPage.chromeIsAddedExactlyOnce", "[contour][geometry]")
{
    auto const cell = imageSize(10, 20);
    auto const page = pageSize(25, 80);
    auto const margins = Margins { .horizontal = 2, .vertical = 2 };

    auto const bare = windowSizeForPage(page, cell, margins, 1.0, Chrome {});
    auto const withBar = windowSizeForPage(page, cell, margins, 1.0, Chrome { .width = 0, .height = 34 });

    CHECK(withBar.width == bare.width);
    CHECK(withBar.height == bare.height + 34);
}

TEST_CASE("WindowGeometry.initialPageSize.runningWinsOverProfile", "[contour][geometry]")
{
    // The spawn-context rule behind the "new tab / split adopts the live window size" fix: a new tab or
    // split pane spawned inside an existing window inherits that window's currently-running page size,
    // regardless of what the profile default is; only a brand-new window (no running size to inherit)
    // falls back to the profile default. This is the ONLY place that decision is made.
    auto const profileDefault = pageSize(25, 80);

    // A resized window that grew to 40x200 -> a new tab inherits 40x200, NOT the 25x80 profile default.
    CHECK(initialPageSize(pageSize(40, 200), profileDefault) == pageSize(40, 200));
    // A window shrunk below the profile default is still the authority (the profile must not enlarge it).
    CHECK(initialPageSize(pageSize(10, 30), profileDefault) == pageSize(10, 30));
    // A running size that happens to equal the default is passed through unchanged.
    CHECK(initialPageSize(profileDefault, profileDefault) == profileDefault);
    // A brand-new window: no running size -> the profile default is honored.
    CHECK(initialPageSize(std::nullopt, profileDefault) == profileDefault);
}

TEST_CASE("WindowGeometry.sizeHintsFor.gutterSurvivesFractionalScale", "[contour][geometry]")
{
    // The gutter is decided in DEVICE pixels -- a fraction of the cell width -- and the base hint is in
    // logical ones. Converting it once, here, is what keeps the two agreeing: unscaling it at the call
    // site first would truncate, and scaling the truncated value back would reserve less than the
    // renderer does, leaving the resize grid off by a pixel.
    auto constexpr Chrome = contour::geometry::Chrome { .width = 0, .height = 0 };
    auto const margins = contour::geometry::Margins { .horizontal = 0, .vertical = 0 };
    auto const cell = imageSize(15, 30);

    for (auto const scale: { 1.0, 1.25, 1.5, 2.0 })
    {
        for (auto const gutterDevicePx: std::views::iota(1, 16))
        {
            CAPTURE(scale, gutterDevicePx);
            auto const bare = sizeHintsFor(cell, margins, scale, Chrome);
            auto const withGutter = sizeHintsFor(cell, margins, scale, Chrome, gutterDevicePx);

            // Whatever the rounding, the reserved strip must cover the device pixels asked for.
            auto const reservedLogical = withGutter.base.width - bare.base.width;
            CHECK(reservedLogical >= 1);
            CHECK(static_cast<double>(reservedLogical) * scale >= static_cast<double>(gutterDevicePx));

            // ... and it is an inset, never part of the resize grid.
            CHECK(withGutter.increment == bare.increment);
        }
    }
}

TEST_CASE("WindowGeometry.sizeHintsFor", "[contour][geometry]")
{
    auto const marginsLogical = Margins { .horizontal = 2, .vertical = 2 };
    auto const chrome = Chrome { .width = 0, .height = 34 };

    SECTION("scale 1.0")
    {
        auto const hints = sizeHintsFor(imageSize(10, 20), marginsLogical, 1.0, chrome);
        // minimum = MinimumTotalPageSize (10 cols x 5 lines) + margins + chrome
        CHECK(hints.minimum == LogicalSize { .width = 104, .height = 138 });
        CHECK(hints.base == LogicalSize { .width = 4, .height = 38 });
        CHECK(hints.increment == LogicalSize { .width = 10, .height = 20 });
    }

    SECTION("scale 2.0 with proportionally scaled device cell: logical hints are scale-invariant")
    {
        auto const hints = sizeHintsFor(imageSize(20, 40), marginsLogical, 2.0, chrome);
        CHECK(hints.minimum == LogicalSize { .width = 104, .height = 138 });
        CHECK(hints.base == LogicalSize { .width = 4, .height = 38 });
        CHECK(hints.increment == LogicalSize { .width = 10, .height = 20 });
    }

    SECTION("fractional scale ceils the increment to cover a whole cell")
    {
        auto const hints = sizeHintsFor(imageSize(9, 19), marginsLogical, 1.25, chrome);
        CHECK(hints.increment == LogicalSize { .width = 8, .height = 16 }); // ceil(9/1.25), ceil(19/1.25)
    }
}

TEST_CASE("WindowGeometry.sizeHintPolicyFor", "[contour][geometry]")
{
    // Minimum size is always safe to apply — it never resizes a mapped window, only floors it.
    for (auto const platform: { SizeHintPlatform::Windows, SizeHintPlatform::MacOS, SizeHintPlatform::Other })
        CHECK(sizeHintPolicyFor(platform).applyMinimum);

    SECTION("macOS omits base + increment (Qt writes base into the NSWindow frame -> invisible window)")
    {
        auto const policy = sizeHintPolicyFor(SizeHintPlatform::MacOS);
        CHECK(policy == SizeHintPolicy { .applyMinimum = true, .applyBase = false, .applyIncrement = false });
    }

    SECTION("Windows applies all three (native character-grid resize snapping)")
    {
        auto const policy = sizeHintPolicyFor(SizeHintPlatform::Windows);
        CHECK(policy == SizeHintPolicy { .applyMinimum = true, .applyBase = true, .applyIncrement = true });
    }

    SECTION("Other (X11/Wayland) applies all three (X11 honors them, Wayland ignores them harmlessly)")
    {
        auto const policy = sizeHintPolicyFor(SizeHintPlatform::Other);
        CHECK(policy == SizeHintPolicy { .applyMinimum = true, .applyBase = true, .applyIncrement = true });
    }
}

TEST_CASE("WindowGeometry.currentSizeHintPlatform.matchesBuildHost", "[contour][geometry]")
{
#ifdef _WIN32
    CHECK(currentSizeHintPlatform() == SizeHintPlatform::Windows);
#elifdef __APPLE__
    CHECK(currentSizeHintPlatform() == SizeHintPlatform::MacOS);
#else
    CHECK(currentSizeHintPlatform() == SizeHintPlatform::Other);
#endif
}

TEST_CASE("WindowGeometry.resolveContentScale.precedence", "[contour][geometry]")
{
    struct Row
    {
        std::optional<double> forcedFontDpi;
        std::optional<double> windowDpr;
        std::optional<double> screenDpr;
        double expected;
    };
    auto const row = GENERATE(
        // Forced font DPI (>= 96) wins outright, even over a real window DPR:
        Row { 144.0, 2.0, 2.0, 1.5 },
        Row { 96.0, 2.0, std::nullopt, 1.0 },
        // Below-96 overrides are ignored (historic contract):
        Row { 72.0, 1.25, std::nullopt, 1.25 },
        // Window DPR wins over the screen guess:
        Row { std::nullopt, 1.5, 2.0, 1.5 },
        // Screen guess as pre-show fallback:
        Row { std::nullopt, std::nullopt, 1.25, 1.25 },
        // Invalid (non-positive) DPRs fall through:
        Row { std::nullopt, 0.0, 1.5, 1.5 },
        Row { std::nullopt, std::nullopt, std::nullopt, 1.0 },
        // Last-resort clamp against nonsensical platform reports:
        Row { std::nullopt, std::nullopt, 0.25, 0.5 });

    CAPTURE(row.forcedFontDpi.value_or(-1), row.windowDpr.value_or(-1), row.screenDpr.value_or(-1));
    CHECK(resolveContentScale(row.forcedFontDpi, row.windowDpr, row.screenDpr) == row.expected);
}

TEST_CASE("WindowGeometry.fitPageToPixels.marginsUseTheDeviceRatioNotTheContentScale", "[contour][geometry]")
{
    // The page fit must scale its margins by the device-pixel RATIO. A forced font DPI changes how large
    // glyphs are rasterized (the content scale); it does not change how many hardware pixels the surface
    // has, so it must not change how many cells fit into it.
    //
    // The numbers are the divergent row from resolveContentScale.precedence above: forceFontDPI 144 on a
    // DPR-2.0 window yields a content scale of 1.5 while the ratio stays 2.0. session::applyResize() used
    // to scale the margins by the former, which both mis-fits the page and disagrees with the identical
    // margin TerminalDisplay bakes into the renderer using the latter.
    constexpr auto Dpr = 2.0;
    constexpr auto ContentScaleUnderForcedDpi = 1.5;
    REQUIRE(resolveContentScale(144.0, Dpr, Dpr) == ContentScaleUnderForcedDpi);

    auto const cell = imageSize(10, 20);
    auto const surface = imageSize(800, 600); // the hardware pixels the surface actually has
    auto const logicalMargin = Margins { .horizontal = 5, .vertical = 5 };
    auto const noClamp = [](vtbackend::PageSize page) {
        return page;
    };

    auto const scaled = [](Margins m, double s) {
        return Margins { .horizontal = static_cast<int>(m.horizontal * s),
                         .vertical = static_cast<int>(m.vertical * s) };
    };

    auto const withRatio = fitPageToPixels(surface, cell, scaled(logicalMargin, Dpr), noClamp);
    auto const withContentScale =
        fitPageToPixels(surface, cell, scaled(logicalMargin, ContentScaleUnderForcedDpi), noClamp);

    // The two disagree, which is precisely why the call site had to pick the right one: a margin scaled by
    // 1.5 instead of 2.0 leaves 5 device pixels unaccounted for on each side.
    CHECK(withRatio.pageMargin.left != withContentScale.pageMargin.left);
    CHECK(withRatio.pageMargin.left == 10);       // 5 logical * DPR 2.0
    CHECK(withContentScale.pageMargin.left == 7); // 5 logical * 1.5, truncated — the bug
}

TEST_CASE("viewportOrigin places a client's view inside a larger shared grid", "[geometry][viewport]")
{
    // A daemon-hosted grid belongs to every attached client, so a client smaller than it shows a
    // WINDOW into it -- and the window has to contain the cursor, or the user types out of sight.
    // The rule is tmux's (tty.c, tty_window_offset1).
    using vtbackend::CellLocation;
    using vtbackend::ColumnOffset;
    using vtbackend::LineOffset;

    auto const at = [](int line, int column) {
        return CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
    };
    auto const grid = PageSize { .lines = LineCount(50), .columns = ColumnCount(200) };

    SECTION("a viewport at least as large as the grid never pans")
    {
        // The letterbox case: the grid is drawn at its own size and the surplus is background.
        CHECK(viewportOrigin(grid, grid, at(49, 199)) == vtbackend::CellLocation {});
        auto const bigger = PageSize { .lines = LineCount(80), .columns = ColumnCount(300) };
        CHECK(viewportOrigin(grid, bigger, at(49, 199)) == vtbackend::CellLocation {});
    }

    SECTION("a cursor already inside the first viewport-worth keeps the grid's origin")
    {
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        CHECK(viewportOrigin(grid, viewport, at(0, 0)) == vtbackend::CellLocation {});
        // The last cell that still fits without moving: one short of the viewport's own extent.
        CHECK(viewportOrigin(grid, viewport, at(9, 39)) == vtbackend::CellLocation {});
    }

    SECTION("the cursor sits on the last visible row, and centred horizontally")
    {
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        auto const origin = viewportOrigin(grid, viewport, at(30, 100));
        // Vertically: row 30 is the bottom row of a 10-row window starting at 21.
        CHECK(origin.line == LineOffset(21));
        // Horizontally: column 100 with 20 columns of context to its left.
        CHECK(origin.column == ColumnOffset(80));
    }

    SECTION("the viewport never runs off the end of the grid")
    {
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        auto const origin = viewportOrigin(grid, viewport, at(49, 199));
        // Clamped to the last full window: 50-10 and 200-40. Without the clamp the view would show
        // rows the grid does not have, which is how a panning terminal draws garbage at the edge.
        CHECK(origin.line == LineOffset(40));
        CHECK(origin.column == ColumnOffset(160));
    }

    SECTION("the axes are decided independently")
    {
        // Wide enough, far too short: the column axis must not pan just because the line axis does.
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(200) };
        auto const origin = viewportOrigin(grid, viewport, at(45, 199));
        CHECK(origin.column == ColumnOffset(0));
        // Row 45 is the bottom row of a 10-row window starting at 36 — short of the 40 the clamp
        // would impose, which only binds once the cursor reaches the last row.
        CHECK(origin.line == LineOffset(36));
    }

    SECTION("a degenerate grid smaller than the viewport still yields a non-negative origin")
    {
        auto const tiny = PageSize { .lines = LineCount(1), .columns = ColumnCount(1) };
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        CHECK(viewportOrigin(tiny, viewport, at(0, 0)) == vtbackend::CellLocation {});
    }
}

// {{{ #2040 -- a split pane's boundary must land on a whole device pixel.
TEST_CASE("WindowGeometry.snapPaneExtentToDevicePixels.lands on a whole device pixel", "[contour][geometry]")
{
    // The property that matters: whatever comes out, multiplying by the DPR yields an integer, so the
    // pane boundary sits on a hardware pixel and the two paths that consume it (the unrounded vertex
    // transform and the qRound()ed scissor) cannot disagree.
    for (auto const dpr: { 1.0, 1.25, 1.5, 2.0, 2.5 })
    {
        for (auto const raw: { 493.5, 494.3, 500.0, 512.7, 987.0 * 0.5 })
        {
            auto const snapped = snapPaneExtentToDevicePixels(raw, dpr);
            auto const inDevicePixels = snapped * dpr;
            CHECK(std::abs(inDevicePixels - std::round(inDevicePixels)) < 1e-9);
        }
    }
}

TEST_CASE("WindowGeometry.snapPaneExtentToDevicePixels.moves by less than one device pixel",
          "[contour][geometry]")
{
    // Snapping must not visibly move the splitter: the correction is a rounding, so it can never
    // exceed half a device pixel. A larger jump would make the divider drift while being dragged.
    for (auto const dpr: { 1.0, 1.5, 2.0 })
    {
        for (auto const raw: { 493.5, 494.3, 512.7, 640.9 })
        {
            auto const snapped = snapPaneExtentToDevicePixels(raw, dpr);
            CHECK(std::abs(snapped - raw) <= (0.5 / dpr) + 1e-9);
        }
    }
}

TEST_CASE("WindowGeometry.snapPaneExtentToDevicePixels.the fractional half-split case from #2040",
          "[contour][geometry]")
{
    // The reported repro: an odd window width split 50/50 at DPR 1 gives each pane a .5 width, so the
    // second pane's origin is fractional and every glyph in it inherits the offset.
    // 987/2 = 493.5 and 993/2 = 496.5; std::round takes halves AWAY from zero, so 494 and 497.
    CHECK(snapPaneExtentToDevicePixels(987.0 * 0.5, 1.0) == Catch::Approx(494.0));
    CHECK(snapPaneExtentToDevicePixels(993.0 * 0.5, 1.0) == Catch::Approx(497.0));

    // At DPR 1 the result is always integral -- the case the artifact was captured at.
    for (auto const width: { 985, 987, 989, 991, 993, 995 })
    {
        auto const snapped = snapPaneExtentToDevicePixels(width * 0.5, 1.0);
        CHECK(snapped == Catch::Approx(std::round(snapped)));
    }
}

TEST_CASE("WindowGeometry.snapPaneExtentToDevicePixels.the split handle needs snapping too",
          "[contour][geometry]")
{
    // Snapping the first pane's EXTENT alone does not put the second pane's ORIGIN on a device pixel:
    // that origin is `firstExtent + handleThickness`, and the default handle is 6 LOGICAL pixels, which
    // is 7.5 device pixels at the very common 125% scale and 10.5 at 175%. Panes on exactly those
    // scales would have kept the #2040 artifact while 100/150/200% were fixed.
    auto constexpr DefaultHandleThickness = 6.0; // vtworkspace::DefaultSplitHandleThickness

    for (auto const dpr: { 1.0, 1.25, 1.5, 1.75, 2.0, 2.5 })
    {
        auto const extent = snapPaneExtentToDevicePixels(987.0 * 0.5, dpr);

        // Snapping the extent alone: the handle carries the fraction straight into the second origin.
        auto const unsnappedOrigin = (extent + DefaultHandleThickness) * dpr;
        auto const handleIsWhole =
            std::abs((DefaultHandleThickness * dpr) - std::round(DefaultHandleThickness * dpr)) < 1e-9;
        CHECK(handleIsWhole
              == (std::abs(unsnappedOrigin - std::round(unsnappedOrigin)) < 1e-9)); // fails at 1.25/1.75

        // Snapping BOTH through the same rule -- what PaneNode.qml now does -- makes it whole at every
        // ratio, which is the property the renderer's two paths need to agree.
        auto const handle = snapPaneExtentToDevicePixels(DefaultHandleThickness, dpr);
        auto const secondOrigin = (extent + handle) * dpr;
        CHECK(std::abs(secondOrigin - std::round(secondOrigin)) < 1e-9);

        // ... and the handle stays visible, never rounded away to nothing.
        CHECK(handle > 0.0);
    }
}

TEST_CASE("WindowGeometry.snapPaneExtentToDevicePixels.a degenerate ratio is a no-op", "[contour][geometry]")
{
    // Nothing sensible to snap to; returning the input unchanged beats dividing by zero.
    CHECK(snapPaneExtentToDevicePixels(493.5, 0.0) == Catch::Approx(493.5));
    CHECK(snapPaneExtentToDevicePixels(493.5, -2.0) == Catch::Approx(493.5));
}

TEST_CASE("WindowGeometry.snapPaneExtentToDevicePixels.an already-snapped extent is unchanged",
          "[contour][geometry]")
{
    // Idempotence: re-snapping must not drift, or a binding that re-evaluates would walk the divider.
    for (auto const dpr: { 1.0, 1.5, 2.0 })
    {
        auto const once = snapPaneExtentToDevicePixels(512.7, dpr);
        CHECK(snapPaneExtentToDevicePixels(once, dpr) == Catch::Approx(once));
    }
}
// }}}
