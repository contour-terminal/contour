// SPDX-License-Identifier: Apache-2.0
//
// Table-driven tests for the pure window-geometry module. The highest-value property here is the
// anti-oscillation roundtrip invariant: a window sized FOR a grid must fit EXACTLY that grid
// (pageSizeForPixels(requiredPixelsForPage(p)) == p) at every cell size, margin and content scale.
// The historic sizing bugs were divergent conversions violating exactly this.

#include <contour/WindowGeometry.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
#if defined(_WIN32)
    CHECK(currentSizeHintPlatform() == SizeHintPlatform::Windows);
#elif defined(__APPLE__)
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
