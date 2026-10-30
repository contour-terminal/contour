// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SizeIndicator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using contour::session::nextSizeIndicatorKind;
using contour::session::SizeIndicatorBurst;
using contour::session::SizeIndicatorGate;
using contour::session::SizeIndicatorKind;
using contour::session::SizeIndicatorSetting;
using contour::session::SizeIndicatorStartupQuiet;

namespace
{

/// A page of @p columns by @p lines.
[[nodiscard]] constexpr vtbackend::PageSize pageOf(int columns, int lines)
{
    return vtbackend::PageSize { .lines = vtbackend::LineCount(lines),
                                 .columns = vtbackend::ColumnCount(columns) };
}

} // namespace

TEST_CASE("sizeIndicator.text", "[sizeindicator]")
{
    using contour::session::sizeIndicatorText;
    // Columns first, as the user reads a terminal's size, with a multiplication sign (U+00D7) between spaces.
    CHECK(sizeIndicatorText(SizeIndicatorKind::PageSize, pageOf(100, 30), 13.0) == "100 × 30");
    CHECK(sizeIndicatorText(SizeIndicatorKind::FontSize, pageOf(100, 30), 13.0) == "13 pt");
    CHECK(sizeIndicatorText(SizeIndicatorKind::FontSize, pageOf(100, 30), 13.5) == "13.5 pt");
}

TEST_CASE("sizeIndicator.announcement", "[sizeindicator]")
{
    using contour::session::sizeIndicatorAnnouncement;
    CHECK(sizeIndicatorAnnouncement(SizeIndicatorKind::PageSize, pageOf(100, 30), 13.0)
          == "100 columns, 30 rows");
    CHECK(sizeIndicatorAnnouncement(SizeIndicatorKind::PageSize, pageOf(1, 1), 13.0) == "1 column, 1 row");
    CHECK(sizeIndicatorAnnouncement(SizeIndicatorKind::PageSize, pageOf(1, 2), 13.0) == "1 column, 2 rows");
    CHECK(sizeIndicatorAnnouncement(SizeIndicatorKind::FontSize, pageOf(100, 30), 13.0) == "13 points");
    CHECK(sizeIndicatorAnnouncement(SizeIndicatorKind::FontSize, pageOf(100, 30), 13.5) == "13.5 points");
    CHECK(sizeIndicatorAnnouncement(SizeIndicatorKind::FontSize, pageOf(100, 30), 1.0) == "1 point");
}

TEST_CASE("sizeIndicator.aFontZoomKeepsItsTextThroughTheResize", "[sizeindicator]")
{
    // The zoom's own resize arrives while its pill shows: the pill keeps saying the size in points.
    CHECK(nextSizeIndicatorKind(
              SizeIndicatorBurst::Showing, SizeIndicatorKind::FontSize, SizeIndicatorKind::PageSize)
          == SizeIndicatorKind::FontSize);
    // Once the burst has settled, a resize reads as a page size again.
    CHECK(nextSizeIndicatorKind(
              SizeIndicatorBurst::Idle, SizeIndicatorKind::FontSize, SizeIndicatorKind::PageSize)
          == SizeIndicatorKind::PageSize);
    // A zoom during a resize takes over.
    CHECK(nextSizeIndicatorKind(
              SizeIndicatorBurst::Showing, SizeIndicatorKind::PageSize, SizeIndicatorKind::FontSize)
          == SizeIndicatorKind::FontSize);
    CHECK(nextSizeIndicatorKind(
              SizeIndicatorBurst::Showing, SizeIndicatorKind::PageSize, SizeIndicatorKind::PageSize)
          == SizeIndicatorKind::PageSize);
}

TEST_CASE("sizeIndicator.gate", "[sizeindicator]")
{
    using namespace std::chrono_literals;
    using contour::session::sizeIndicatorGate;
    CHECK(sizeIndicatorGate(SizeIndicatorStartupQuiet, SizeIndicatorSetting::On) == SizeIndicatorGate::Quiet);
    CHECK(sizeIndicatorGate(SizeIndicatorStartupQuiet + 1ms, SizeIndicatorSetting::On)
          == SizeIndicatorGate::Open);
    CHECK(sizeIndicatorGate(0s, SizeIndicatorSetting::Off) == SizeIndicatorGate::Quiet);
    CHECK(sizeIndicatorGate(SizeIndicatorStartupQuiet + 1ms, SizeIndicatorSetting::Off)
          == SizeIndicatorGate::Quiet);
    CHECK(sizeIndicatorGate(24h, SizeIndicatorSetting::Off) == SizeIndicatorGate::Quiet);
}
