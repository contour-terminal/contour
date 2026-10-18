// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Times.hpp>
#include <core/Utils.hpp>
#include <core/cli/App.hpp>
#include <core/testing/Environment.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
using namespace std;
using namespace std::chrono_literals;
using vtbackend::CellLocation;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::MockTerm;
using vtbackend::Modifier;
using vtbackend::PageSize;
using vtbackend::SmoothScrollResult;
using namespace vtbackend::test;

// NOLINTBEGIN(misc-const-correctness)

TEST_CASE("Terminal.smoothScrollExtraLines.zero_when_no_offset", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    CHECK(*terminal.smoothScrollExtraLines() == 0);
}

TEST_CASE("Terminal.smoothScrollExtraLines.one_when_offset_nonzero", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    terminal.viewport().setPixelOffset(5.0f);
    CHECK(*terminal.smoothScrollExtraLines() == 1);
}

TEST_CASE("Terminal.screenTransitionProgress.no_transition_returns_1", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    // No transition active, should return 1.0 (complete).
    CHECK(terminal.screenTransitionProgress() == 1.0f);
    CHECK_FALSE(terminal.isScreenTransitionActive());
}

TEST_CASE("Terminal.applySmoothScrollPixelDelta.accumulates_subline_offset", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Write enough lines to generate history.
    for (auto i = 0; i < 14; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // A delta smaller than one cell height should only accumulate pixel offset.
    auto const result = terminal.applySmoothScrollPixelDelta(5.0f);
    CHECK(result == SmoothScrollResult::Applied);
    CHECK(terminal.smoothScrollPixelOffset() == 5.0f);
    CHECK(terminal.viewport().scrollOffset().value == 0);
}

TEST_CASE("Terminal.applySmoothScrollPixelDelta.converts_full_cell_to_scroll", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 14; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    auto const cellHeight = terminal.cellPixelSize().height.as<float>();
    auto const result = terminal.applySmoothScrollPixelDelta(cellHeight + 3.0f);
    CHECK(result == SmoothScrollResult::Applied);
    CHECK(terminal.viewport().scrollOffset().value == 1);
    CHECK(terminal.smoothScrollPixelOffset() == Catch::Approx(3.0f));
}

TEST_CASE("Terminal.applySmoothScrollPixelDelta.clamps_at_top_of_history", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 14; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Apply a delta much larger than all available history.
    auto const result = terminal.applySmoothScrollPixelDelta(100000.0f);
    CHECK(result == SmoothScrollResult::Applied);
    // Scroll offset should be clamped to max history.
    auto const maxOffset = terminal.primaryScreen().historyLineCount();
    CHECK(terminal.viewport().scrollOffset().value == maxOffset.as<int>());
    CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
}

TEST_CASE("Terminal.applySmoothScrollPixelDelta.returns_disabled_on_alternate_screen", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Switch to alternate screen.
    mc.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());

    auto const result = terminal.applySmoothScrollPixelDelta(10.0f);
    CHECK(result == SmoothScrollResult::Disabled);
}

TEST_CASE("Terminal.momentumScroll.starts_on_end_with_velocity", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Simulate touchpad gesture: Begin, several Updates, then End.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    CHECK(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.velocity_computation_is_correct", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 20; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // 3 Updates each 10ms apart, each with 20px delta.
    // Expected velocity = (20 + 20) / (30ms - 10ms) = 40 / 0.02 = 2000 px/s
    // (The oldest sample's delta is excluded from the sum.)
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 20.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 20.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 20.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Apply one tick at ~16ms after End and verify the scroll offset advanced.
    // With v=2000 px/s and dt=16ms: pixelDelta = 2000 * 0.016 = 32px.
    // With cellHeight=20, that's 1 full line + 12px remainder.
    auto const offsetBefore = terminal.viewport().scrollOffset().value;
    terminal.tick(ClockBase + 56ms);
    auto const offsetAfter = terminal.viewport().scrollOffset().value;
    CHECK(offsetAfter > offsetBefore);
}

TEST_CASE("Terminal.momentumScroll.no_start_below_threshold", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Very small, slow deltas — velocity below startThreshold (50 px/s).
    // velocity = 0.5 / 0.1 = 5 px/s
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 0.5f, ClockBase + 100ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 0.5f, ClockBase + 200ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 300ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.no_start_with_single_sample", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Only a single Update — VelocityTracker needs at least 2 samples.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 100.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 20ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.decelerates_over_ticks", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 20; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Start momentum with fast gesture.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Track scroll offset + pixel offset through several frames.
    auto const offsetAfterStart = terminal.viewport().scrollOffset().value;
    auto const pixelOffsetAfterStart = terminal.smoothScrollPixelOffset();

    // First frame of momentum.
    terminal.tick(ClockBase + 56ms);
    auto const offset1 = terminal.viewport().scrollOffset().value;
    auto const pixel1 = terminal.smoothScrollPixelOffset();

    // Second frame.
    terminal.tick(ClockBase + 72ms);
    auto const offset2 = terminal.viewport().scrollOffset().value;
    auto const pixel2 = terminal.smoothScrollPixelOffset();

    // Third frame.
    terminal.tick(ClockBase + 88ms);
    auto const offset3 = terminal.viewport().scrollOffset().value;

    // Scroll offset should advance (or pixel accumulate) over time.
    auto const totalScroll1 = (static_cast<float>(offset1) * 20.0f) + pixel1;
    auto const totalScroll0 = (static_cast<float>(offsetAfterStart) * 20.0f) + pixelOffsetAfterStart;
    CHECK(totalScroll1 > totalScroll0);

    // Overall scroll should only increase (deceleration, not reversal).
    auto const totalScroll2 = (static_cast<float>(offset2) * 20.0f) + pixel2;
    CHECK(totalScroll2 >= totalScroll1);

    // Scroll offset should have advanced by at least one line after several ticks.
    CHECK(offset3 >= offsetAfterStart);
}

TEST_CASE("Terminal.momentumScroll.stops_at_min_velocity", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Advance time far enough for velocity to decay below threshold.
    // With FrictionDecayPerSecond=0.05 and velocity ~3000 px/s, after ~2.5s velocity ≈ 3.7 px/s < 10.
    terminal.tick(ClockBase + 3030ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.cancelled_by_begin", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Start momentum.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Let momentum run for one tick.
    terminal.tick(ClockBase + 46ms);
    REQUIRE(terminal.isMomentumScrollActive());

    // New gesture begins — should cancel momentum.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase + 50ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.single_notch_glides_over_frames", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    auto constexpr CellHeight = 20.0f;
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // One notch worth of pixels: 9 lines * 20px = 180px (matches helper.cpp's angle-to-pixel math).
    auto constexpr NotchPixels = 180.0f;
    terminal.injectWheelMomentum(NotchPixels, ClockBase);
    REQUIRE(terminal.isMomentumScrollActive());

    // Frame 1: the glide must NOT jump the full notch distance instantly.
    terminal.tick(ClockBase + 16ms);
    auto const afterFrame1 = totalScrollPixels(terminal, CellHeight);
    CHECK(afterFrame1 > 0.0f);
    CHECK(afterFrame1 < NotchPixels);

    // Subsequent frames advance monotonically toward the target.
    terminal.tick(ClockBase + 33ms);
    auto const afterFrame2 = totalScrollPixels(terminal, CellHeight);
    CHECK(afterFrame2 >= afterFrame1);

    // After enough time the glide settles near the intended distance and stops.
    for (auto t = 50; t <= 600; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    auto const settled = totalScrollPixels(terminal, CellHeight);
    CHECK_FALSE(terminal.isMomentumScrollActive());
    // Lands within one line of the intended distance (tuned impulse, not floaty).
    CHECK(settled == Catch::Approx(NotchPixels).margin(CellHeight));
}

TEST_CASE("Terminal.wheelGlide.rapid_notches_accumulate", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 60 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 50; ++i)
        mc.writeToScreen("line\r\n");

    auto constexpr CellHeight = 20.0f;
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Two notches in quick succession accumulate into one longer glide.
    terminal.injectWheelMomentum(180.0f, ClockBase);
    terminal.injectWheelMomentum(180.0f, ClockBase + 8ms);
    REQUIRE(terminal.isMomentumScrollActive());

    for (auto t = 16; t <= 800; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    auto const settled = totalScrollPixels(terminal, CellHeight);
    CHECK_FALSE(terminal.isMomentumScrollActive());
    // Both notches contribute; total lands near their sum (within a line).
    CHECK(settled == Catch::Approx(360.0f).margin(CellHeight));
}

TEST_CASE("Terminal.wheelGlide.direction_reversal", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 60 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 50; ++i)
        mc.writeToScreen("line\r\n");

    auto constexpr CellHeight = 20.0f;
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Scroll up, then reverse before the first glide settles: velocity is signed-accumulated.
    terminal.injectWheelMomentum(180.0f, ClockBase);
    terminal.tick(ClockBase + 8ms);
    terminal.injectWheelMomentum(-120.0f, ClockBase + 12ms);

    for (auto t = 20; t <= 800; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    auto const settled = totalScrollPixels(terminal, CellHeight);
    CHECK_FALSE(terminal.isMomentumScrollActive());
    // Net displacement trends toward +60px (up), never overshooting far past it.
    CHECK(settled > 0.0f);
    CHECK(settled < 180.0f);
}

TEST_CASE("Terminal.wheelGlide.clamps_at_history_top", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    auto const historyLines = terminal.primaryScreen().historyLineCount().as<int>();
    REQUIRE(historyLines > 0);

    // Inject far more than the scrollable distance; the glide must stop at the wall, not spin.
    terminal.injectWheelMomentum(static_cast<float>(historyLines + 20) * 20.0f, ClockBase);

    for (auto t = 16; t <= 800; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });

    CHECK(terminal.viewport().scrollOffset().value == historyLines);
    CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.inactive_on_alt_screen", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Switch to alternate screen; the wheel glide must not arm there.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isAlternateScreen());

    auto const offsetBefore = terminal.viewport().scrollOffset().value;
    terminal.injectWheelMomentum(180.0f, ClockBase);

    CHECK_FALSE(terminal.isMomentumScrollActive());
    CHECK(terminal.viewport().scrollOffset().value == offsetBefore);
}

TEST_CASE("Terminal.wheelGlide.cancelled_by_reset", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.injectWheelMomentum(180.0f, ClockBase);
    REQUIRE(terminal.isMomentumScrollActive());

    // resetSmoothScroll (called on resize / page switch / scroll-to-bottom / new output) cancels it.
    terminal.resetSmoothScroll();
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.gated_on_smoothScrolling_only", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Independent of momentumScrolling: with momentum off but smooth on, the wheel still glides.
    terminal.settings().smoothScrolling = true;
    terminal.settings().momentumScrolling = false;
    terminal.injectWheelMomentum(180.0f, ClockBase);
    CHECK(terminal.isMomentumScrollActive());
    terminal.cancelMomentumScroll();

    // With smooth scrolling off, the wheel glide does not arm (legacy line path handles it).
    terminal.settings().smoothScrolling = false;
    terminal.injectWheelMomentum(180.0f, ClockBase);
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.nextRender_schedules_while_active", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.injectWheelMomentum(180.0f, ClockBase);
    terminal.tick(ClockBase + 16ms);
    REQUIRE(terminal.isMomentumScrollActive());

    // While a glide is active, nextRender() schedules a frame at (or below) the refresh interval.
    auto const wakeup = terminal.nextRender();
    REQUIRE(wakeup.has_value());
    CHECK(wakeup->count() > 0);
}

TEST_CASE("Terminal.wheelGlide.opposing_notches_do_not_spin_forever", "[terminal]")
{
    // Regression: two exactly-opposing notches delivered before a frame tick accumulate to a net
    // velocity of exactly 0.0f. Arming at velocity 0 would anchor the fraction-of-seed stop
    // threshold to 0, so shouldStop() could never fire — the glide would stay active forever and
    // keep waking the render loop (continuous CPU/battery drain). It must instead refuse to arm.
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // First notch arms a glide; the exactly-opposing second notch (same magnitude, same time point,
    // no tick() in between) drives the accumulated velocity to precisely 0.
    REQUIRE(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Applied);
    REQUIRE(terminal.isMomentumScrollActive());
    auto const result = terminal.injectWheelMomentum(-180.0f, ClockBase);

    // The degenerate notch neither arms nor leaves the previous glide running; the caller is told to
    // fall through to the legacy line path.
    CHECK(result == SmoothScrollResult::InvalidCellSize);
    CHECK_FALSE(terminal.isMomentumScrollActive());

    // Advance well past any plausible glide duration: still inactive, and nextRender() no longer
    // requests wake-ups on account of momentum.
    for (auto t = 16; t <= 2000; t += 16)
        terminal.tick(ClockBase + chrono::milliseconds { t });
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.wheelGlide.reports_result_for_caller_fallthrough", "[terminal]")
{
    // The SmoothScrollResult return value is the seam helper.cpp uses to decide whether to consume
    // the wheel notch (Applied) or fall through to the legacy line-based scroll (anything else).
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 20 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 12; ++i)
        mc.writeToScreen("line\r\n");

    SECTION("unknown cell size (not laid out yet) -> InvalidCellSize, no arm")
    {
        // No setCellPixelSize(): the display has not laid out, so cell height is still 0.
        REQUIRE(terminal.cellPixelSize().height.as<int>() == 0);
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::InvalidCellSize);
        CHECK_FALSE(terminal.isMomentumScrollActive());
    }

    SECTION("smooth scrolling disabled -> Disabled, no arm")
    {
        terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
        terminal.settings().smoothScrolling = false;
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Disabled);
        CHECK_FALSE(terminal.isMomentumScrollActive());
    }

    SECTION("alternate screen -> Disabled, no arm")
    {
        terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
        mc.writeToScreen("\033[?1049h");
        REQUIRE(terminal.isAlternateScreen());
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Disabled);
        CHECK_FALSE(terminal.isMomentumScrollActive());
    }

    SECTION("healthy notch -> Applied, arms a glide")
    {
        terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
        CHECK(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Applied);
        CHECK(terminal.isMomentumScrollActive());
    }
}

TEST_CASE("Terminal.momentumScroll.stray_update_cancels_active_glide", "[terminal]")
{
    // Regression: a touchpad Update phase arriving while a wheel glide is still in flight, without a
    // preceding Begin (the only phase that used to cancel momentum), would let BOTH the immediate
    // apply of the Update and the live glide move the viewport -> double-scroll. The first sample of
    // a fresh gesture (tracker still empty) must cancel any active momentum first.
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Arm a wheel glide, let it run for a frame so it is genuinely mid-flight.
    REQUIRE(terminal.injectWheelMomentum(180.0f, ClockBase) == SmoothScrollResult::Applied);
    terminal.tick(ClockBase + 16ms);
    REQUIRE(terminal.isMomentumScrollActive());

    // A stray Update (no Begin) is the first sample of a new gesture -> it must cancel the glide.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 24ms);
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.cancelled_by_resize", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Resize cancels momentum via resetSmoothScroll().
    terminal.resizeScreen(PageSize { LineCount { 5 }, ColumnCount { 10 } });

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.disabled_when_setting_off", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Disable momentum scrolling.
    terminal.settings().momentumScrolling = false;

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.disabled_when_smooth_scrolling_off", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Momentum requires smooth scrolling to be enabled.
    terminal.settings().smoothScrolling = false;

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.noPhase_never_triggers_momentum", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Mouse wheel events have NoPhase — should never trigger momentum.
    terminal.handleScrollPhase(vtbackend::ScrollPhase::NoPhase, 100.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::NoPhase, 100.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::NoPhase, 100.0f, ClockBase + 20ms);

    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.momentumScroll.nextRender_schedules_during_active", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // nextRender should return a value during active momentum.
    auto const next = terminal.nextRender();
    CHECK(next.has_value());
}

TEST_CASE("Terminal.momentumScroll.repeated_gestures_work_independently", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 30 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 30; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // --- First gesture: scroll up into history ---
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 30ms);
    // Apply the scroll deltas.
    for (auto i = 0; i < 3; ++i)
        terminal.applySmoothScrollPixelDelta(40.0f);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());
    auto const offsetAfterFirstEnd = terminal.viewport().scrollOffset().value;

    // Let momentum run for a bit.
    terminal.tick(ClockBase + 56ms);
    terminal.tick(ClockBase + 72ms);
    auto const offsetAfterFirstMomentum = terminal.viewport().scrollOffset().value;
    CHECK(offsetAfterFirstMomentum >= offsetAfterFirstEnd);
    REQUIRE(terminal.isMomentumScrollActive());

    // --- Second gesture: Begin cancels first momentum ---
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase + 100ms);
    CHECK_FALSE(terminal.isMomentumScrollActive());

    auto const offsetBeforeSecond = terminal.viewport().scrollOffset().value;

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 60.0f, ClockBase + 110ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 60.0f, ClockBase + 120ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 60.0f, ClockBase + 130ms);
    for (auto i = 0; i < 3; ++i)
        terminal.applySmoothScrollPixelDelta(60.0f);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 140ms);

    // Second gesture should start its own momentum.
    REQUIRE(terminal.isMomentumScrollActive());

    // Momentum from second gesture should still scroll further.
    terminal.tick(ClockBase + 156ms);
    auto const offsetAfterSecondMomentum = terminal.viewport().scrollOffset().value;
    CHECK(offsetAfterSecondMomentum >= offsetBeforeSecond);
}

TEST_CASE("Terminal.momentumScroll.rapid_repeated_gestures", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 50 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 50; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Simulate 3 rapid gestures in quick succession without letting momentum settle.
    auto t = ClockBase;
    for (auto gesture = 0; gesture < 3; ++gesture)
    {
        terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, t);
        if (gesture > 0)
            CHECK_FALSE(terminal.isMomentumScrollActive()); // Begin cancels previous momentum.

        terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, t + 8ms);
        terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, t + 16ms);
        terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 30.0f, t + 24ms);
        for (auto i = 0; i < 3; ++i)
            terminal.applySmoothScrollPixelDelta(30.0f);
        terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, t + 32ms);

        CHECK(terminal.isMomentumScrollActive());

        // Let one tick of momentum run before next gesture.
        t += 50ms;
        terminal.tick(t);
    }

    // After all three gestures, momentum from the last one should still be active.
    CHECK(terminal.isMomentumScrollActive());

    // Scroll should have advanced into history.
    CHECK(terminal.viewport().scrollOffset().value > 0);
}

TEST_CASE("Terminal.momentumScroll.scroll_position_advances_correctly", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 40; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    // Known velocity: 3 Updates, 40px each, 10ms apart.
    // velocity = (40 + 40) / 0.02 = 4000 px/s
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 40.0f, ClockBase + 30ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 40ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Run momentum frames at 16ms intervals until it stops.
    auto lastActive = true;
    auto frameCount = 0;
    for (auto t = ClockBase + 56ms; lastActive && frameCount < 200; t += 16ms, ++frameCount)
    {
        terminal.tick(t);
        lastActive = terminal.isMomentumScrollActive();
    }

    // Momentum should have eventually stopped.
    CHECK_FALSE(terminal.isMomentumScrollActive());

    // Should have scrolled at least several lines (4000 px/s is a brisk swipe).
    CHECK(terminal.viewport().scrollOffset().value > 0);
}

TEST_CASE("Terminal.momentumScroll.cancelled_by_alternate_screen", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 10 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    for (auto i = 0; i < 8; ++i)
        mc.writeToScreen("line\r\n");

    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    terminal.handleScrollPhase(vtbackend::ScrollPhase::Begin, 0.0f, ClockBase);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 10ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::Update, 50.0f, ClockBase + 20ms);
    terminal.handleScrollPhase(vtbackend::ScrollPhase::End, 0.0f, ClockBase + 30ms);

    REQUIRE(terminal.isMomentumScrollActive());

    // Switch to alternate screen (e.g. vim) — cancels momentum via resetSmoothScroll.
    mc.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());
    CHECK_FALSE(terminal.isMomentumScrollActive());
}

TEST_CASE("Terminal.cursorMotionAnimation.starts_on_position_change", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 20 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    // Ensure animation is enabled (default is 80ms).
    REQUIRE(terminal.settings().cursorMotionAnimationDuration.count() > 0);

    // Tick far enough from epoch so the refresh interval (41ms) is satisfied.
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Move cursor by writing a character.
    mc.writeToScreen("A");
    terminal.tick(ClockBase + 200ms);
    terminal.ensureFreshRenderBuffer();

    auto const renderBuffer = terminal.renderBuffer();
    REQUIRE(renderBuffer.get().cursor.has_value());
    auto const& cursor = *renderBuffer.get().cursor;
    CHECK(cursor.animateFrom.has_value());
    CHECK(cursor.animationProgress < 1.0f);
}

TEST_CASE("Terminal.cursorMotionAnimation.chains_midanimation", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 20 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    REQUIRE(terminal.settings().cursorMotionAnimationDuration.count() > 0);

    // Tick far enough from epoch so the refresh interval is satisfied.
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // A far move, ten columns, started by the frame at 200 ms. A motion lasts at least three frames, 100 ms
    // at MockTerm's 30 Hz.
    mc.writeToScreen("\033[1;11H");
    terminal.tick(ClockBase + 200ms);
    terminal.ensureFreshRenderBuffer();
    {
        auto const frame = terminal.renderBuffer();
        REQUIRE(frame.get().cursor.has_value());
        REQUIRE(frame.get().cursor->animateFrom == vtbackend::FractionalCellLocation {});
        REQUIRE(frame.get().cursor->animationProgress < 1.0f);
    } // Release RenderBufferRef lock before next render cycle.

    // Half-way along, the cursor moves on, and the frame at that instant chains: a passive build fills as the
    // batch ends, the others when asked.
    terminal.tick(ClockBase + 250ms);
    mc.writeToScreen("\033[3;5H");
    terminal.ensureFreshRenderBuffer();

    auto const frame = terminal.renderBuffer();
    REQUIRE(frame.get().cursor.has_value());
    auto const& cursor = *frame.get().cursor;

    // The chained motion starts from where the first one had got to, strictly between its ends: not from its
    // start, and not from its end, where a fresh motion would start.
    REQUIRE(cursor.animateFrom.has_value());
    CHECK(cursor.animateFrom->line == 0.0f);
    CHECK(cursor.animateFrom->column > 0.0f);
    CHECK(cursor.animateFrom->column < 10.0f);
    CHECK(cursor.animationProgress < 1.0f);
}

TEST_CASE("Terminal.cursorMotionAnimation.aChainStartsWhereTheCursorIsDrawn", "[terminal]")
{
    // A moving cursor is drawn part of the way between two cells. A motion chained half-way along starts from
    // there, not from the whole cell before it, which snapped the drawn cursor back.
    auto mc = MockTerm { ColumnCount { 20 }, LineCount { 4 } };
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();
    mc.writeToScreen("\033[1;11H");
    terminal.tick(ClockBase + 200ms);
    terminal.ensureFreshRenderBuffer(); // starts the motion from (0, 0) to (0, 10)

    // Where the cursor is drawn at 250 ms, as the renderer interpolates: from + progress * (to - from).
    terminal.tick(ClockBase + 250ms);
    auto const progress =
        terminal.cursorAnimationProgress(CellLocation { .line = LineOffset(0), .column = ColumnOffset(10) });
    REQUIRE(progress > 0.0f);
    REQUIRE(progress < 1.0f);
    auto const drawnColumn = progress * 10.0f;

    mc.writeToScreen("\033[3;5H");
    terminal.ensureFreshRenderBuffer();

    auto const frame = terminal.renderBuffer();
    REQUIRE(frame.get().cursor.has_value());
    auto const& cursor = *frame.get().cursor;
    REQUIRE(cursor.animateFrom.has_value());
    CHECK(cursor.animateFrom->line == Catch::Approx(0.0f));
    CHECK(cursor.animateFrom->column == Catch::Approx(drawnColumn));
}

TEST_CASE("Terminal.screenTransition.activates_on_screen_switch", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    // Ensure fade transition is configured.
    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    // Write some text to the primary screen.
    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Switch to alternate screen.
    mc.writeToScreen("\033[?1049h");

    CHECK(terminal.isScreenTransitionActive());
}

TEST_CASE("Terminal.screenTransition.fades_out_blends_to_background", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    // Write text so there are non-trivial cells to blend.
    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Switch to alternate screen, starting the transition.
    // setScreen() records _currentTime (ClockBase + 100ms) as startTime.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick to 50ms past startTime (ClockBase + 150ms), i.e. 25% of the 200ms duration.
    terminal.tick(ClockBase + 150ms);
    terminal.ensureFreshRenderBuffer();

    // The transition is still active and in fade-out phase.
    auto const progress = terminal.screenTransitionProgress();
    CHECK(progress > 0.0f);
    CHECK(progress < 0.5f);
}

TEST_CASE("Terminal.screenTransition.fadeout_cell_colors_blend_toward_background", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    // Set a known foreground color via SGR so snapshot cells have non-default foreground.
    // ESC[38;2;255;0;0m sets foreground to bright red.
    mc.writeToScreen("\033[38;2;255;0;0mHello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Capture the pre-transition foreground color of the first rendered cell.
    auto preFg = vtbackend::RGBColor {};
    {
        auto const buf = terminal.renderBuffer();
        REQUIRE(!buf.get().cells.empty());
        preFg = buf.get().cells.front().attributes.foregroundColor;
    }
    // The foreground should be close to red (255, 0, 0).
    REQUIRE(preFg.red > 200);

    auto const defaultBg = terminal.colorPalette().defaultBackground;

    // Switch to alternate screen, starting the fade transition.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick to 25% of the 200ms duration (fade-out phase: progress < 0.5).
    // At 25% overall, the fade-out factor is 0.5 (progress * 2).
    terminal.tick(ClockBase + 150ms);
    terminal.ensureFreshRenderBuffer();

    auto const progress = terminal.screenTransitionProgress();
    REQUIRE(progress > 0.0f);
    REQUIRE(progress < 0.5f);

    auto const buf = terminal.renderBuffer();
    REQUIRE(!buf.get().cells.empty());

    auto const& blendedFg = buf.get().cells.front().attributes.foregroundColor;

    // During fade-out, the foreground should be blended toward defaultBg.
    // The red channel should have decreased from the original value toward defaultBg.red.
    // The green/blue channels should have moved toward defaultBg.green/blue.
    if (preFg.red > defaultBg.red)
        CHECK(blendedFg.red < preFg.red);
    else
        CHECK(blendedFg.red > preFg.red);

    // Verify that the blended color is between the original and the default background.
    auto const isRedBetween = (blendedFg.red >= std::min(preFg.red, defaultBg.red))
                              && (blendedFg.red <= std::max(preFg.red, defaultBg.red));
    auto const isGreenBetween = (blendedFg.green >= std::min(preFg.green, defaultBg.green))
                                && (blendedFg.green <= std::max(preFg.green, defaultBg.green));
    auto const isBlueBetween = (blendedFg.blue >= std::min(preFg.blue, defaultBg.blue))
                               && (blendedFg.blue <= std::max(preFg.blue, defaultBg.blue));
    CHECK(isRedBetween);
    CHECK(isGreenBetween);
    CHECK(isBlueBetween);
}

TEST_CASE("Terminal.screenTransition.finalizes_after_duration", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // setScreen() records _currentTime (ClockBase + 100ms) as startTime.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick past the full duration (startTime + 200ms = ClockBase + 300ms).
    terminal.tick(ClockBase + 400ms);
    CHECK_FALSE(terminal.isScreenTransitionActive());
}

TEST_CASE("Terminal.screenTransition.reaches_fade_in_phase", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 10 }, LineCount { 4 } };
    auto& terminal = mc.terminal;

    auto constexpr ClockBase = chrono::steady_clock::time_point();
    terminal.tick(ClockBase);

    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 200ms;

    mc.writeToScreen("Hello");
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();

    // Switch to alternate screen, starting the transition at _currentTime = ClockBase + 100ms.
    mc.writeToScreen("\033[?1049h");
    REQUIRE(terminal.isScreenTransitionActive());

    // Tick to 60% of the 200ms duration (120ms past startTime = ClockBase + 220ms).
    terminal.tick(ClockBase + 220ms);
    terminal.ensureFreshRenderBuffer();

    auto const progress = terminal.screenTransitionProgress();
    CHECK(progress > 0.5f);
    CHECK(terminal.isScreenTransitionActive());
}

TEST_CASE("Terminal.TopAnchoredRegion.PartialScrollKeepsViewportFixed", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(8) }, LineCount(20) };
    auto& terminal = mc.terminal;

    // Generate scrollback so the viewport can be scrolled up.
    mc.writeToScreen("h1\r\nh2\r\nh3\r\nh4\r\nh5\r\nh6\r\nh7\r\nh8\r\n");
    terminal.viewport().scrollUp(LineCount(3));
    REQUIRE(terminal.viewport().scrolled());
    auto const scrollOffsetBefore = terminal.viewport().scrollOffset();

    // Top-anchored partial region (rows 1..3), cursor at the region bottom.
    mc.writeToScreen("\033[1;3r");
    mc.writeToScreen("\033[3;1H");

    // CSI S (SU) scrolls the region up; the viewport the user scrolled to must
    // not jump as a side effect.
    mc.writeToScreen("\033[S");

    CHECK(terminal.viewport().scrollOffset() == scrollOffsetBefore);
}

TEST_CASE("Terminal.TopAnchoredRegion.PartialScrollDoesNotMoveNormalModeCursor", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(8) }, LineCount(20) };
    auto& terminal = mc.terminal;

    mc.writeToScreen("r1\r\nr2\r\nr3\r\nr4\r\nr5\r\nr6");
    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto const cursorLineBefore = terminal.normalModeCursorPosition().line;

    // Top-anchored partial region (rows 1..3), cursor at region bottom, then IND.
    mc.writeToScreen("\033[1;3r");
    mc.writeToScreen("\033[3;1H");
    mc.writeToScreen("\033D");

    CHECK(terminal.normalModeCursorPosition().line == cursorLineBefore);
}

TEST_CASE("Terminal.TopAnchoredRegion.ScrollCountMatchesScrolledLines", "[terminal]")
{
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(8) }, LineCount(2) };
    auto& terminal = mc.terminal;

    // Fill history to capacity (2 lines) so further scrolls have no headroom.
    mc.writeToScreen("a\r\nb\r\nc\r\nd\r\ne\r\nf\r\n");
    terminal.viewport().scrollUp(LineCount(1));
    REQUIRE(terminal.viewport().scrolled());
    auto const scrollOffsetBefore = terminal.viewport().scrollOffset();

    // Top-anchored partial region, cursor at region bottom, scroll it.
    mc.writeToScreen("\033[1;2r");
    mc.writeToScreen("\033[2;1H");
    mc.writeToScreen("\033D");

    // The viewport must not drift by the history/scroll-count mismatch.
    CHECK(terminal.viewport().scrollOffset() == scrollOffsetBefore);
}

TEST_CASE("Terminal.Wheel.AltScreen.NoProtocol.emits_cursor_keys", "[terminal]")
{
    // #1951: on the alt screen (less/most/man) with no mouse protocol, a wheel notch must
    // translate into cursor keys so the pager scrolls. Before the fix, replyData() stayed empty.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    auto constexpr UiHandledHint = false;
    mock.terminal.tick(1s);

    // Enter the alternate screen (DECSET ?1049).
    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);
    REQUIRE(mock.terminal.isAlternateScreen());

    mock.resetReplyData();
    auto const handledDown = mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, UiHandledHint);
    CHECK(handledDown == Handled { true });
    CHECK(e(mock.replyData()) == e("\033[B")); // default multiplier is 1 in MockTerm

    mock.resetReplyData();
    auto const handledUp = mock.terminal.sendMousePressEvent(
        Modifier::None, MouseButton::WheelUp, mock.terminal.currentMousePosition(), NoPixel, UiHandledHint);
    CHECK(handledUp == Handled { true });
    CHECK(e(mock.replyData()) == e("\033[A"));
}

TEST_CASE("Terminal.Wheel.AltScreen.AppCursorKeys.emits_SS3", "[terminal]")
{
    // DECCKM (?1h) on the alternate screen selects application cursor keys (SS3 form).
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h"); // alt screen
    mock.writeToScreen("\033[?1h");    // DECCKM: application cursor keys
    mock.terminal.tick(1s);
    REQUIRE(mock.terminal.isAlternateScreen());

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { true });
    CHECK(e(mock.replyData()) == e("\033OB"));
}

TEST_CASE("Terminal.Wheel.AltScreen.DECSET1007.emits_cursor_keys", "[terminal]")
{
    // DECSET ?1007 (alternate scroll) on the alternate screen: the wheel sends cursor keys, in DECCKM's form.
    // It was once a no-op because of the mouse-protocol gate.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h"); // alt screen
    mock.writeToScreen("\033[?1007h"); // alternate scroll on (as it already is from power-on)
    mock.terminal.tick(1s);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { true });
    CHECK(e(mock.replyData()) == e("\033[B"));
}

TEST_CASE("Terminal.Wheel.AltScreen.AppTracking.passes_through_as_SGR", "[terminal]")
{
    // With an app-enabled protocol (`less --mouse`, `ov`: ?1000h/?1002h + SGR ?1006h), the wheel
    // must be reported to the app, NOT translated to cursor keys. Guards the passthrough case.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");                       // alt screen
    mock.writeToScreen("\033[?1000h\033[?1002h\033[?1006h"); // less --mouse style
    mock.terminal.tick(1s);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { true });
    // SGR mouse report, not a cursor key.
    CHECK(e(mock.replyData()).starts_with(e("\033[<")));
    CHECK(!e(mock.replyData()).contains(e("\033[B")));
}

TEST_CASE("Terminal.Wheel.PrimaryScreen.NoProtocol.local_scroll", "[terminal]")
{
    // Primary screen, no protocol: wheel mode is Default, so the backend does not handle it
    // (the frontend scrolls scrollback). No bytes are sent and the event is reported unhandled.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);
    REQUIRE(mock.terminal.isPrimaryScreen());

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { false });
    CHECK(mock.replyData().empty());
}

TEST_CASE("Terminal.Wheel.AltScreen.ShiftBypass.not_handled", "[terminal]")
{
    // Holding the bypass modifier (Shift by default) must let the wheel event fall through to
    // the frontend's Shift+Wheel binding (page-scroll), so the backend reports it unhandled
    // and emits nothing.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::Shift, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { false });
    CHECK(mock.replyData().empty());
}

TEST_CASE("Terminal.Wheel.AltScreen.ViNormalMode.no_cursor_keys", "[terminal]")
{
    // In Vi normal mode the wheel must not inject cursor keys into the app.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);
    mock.terminal.inputHandler().setMode(ViMode::Normal);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { false });
    CHECK(mock.replyData().empty());

    mock.terminal.inputHandler().setMode(ViMode::Insert);
}

TEST_CASE("Terminal.Wheel.AltScreen.ScrollMultiplier.repeats_cursor_keys", "[terminal]")
{
    // With a scroll multiplier of 3, one alt-screen wheel notch emits 3 cursor keys, matching
    // the primary-screen scrollback feel.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h");
    mock.terminal.tick(1s);
    mock.terminal.setMouseWheelScrollMultiplier(LineCount(3));

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { true });
    CHECK(e(mock.replyData()) == e("\033[B\033[B\033[B"));
}

TEST_CASE("Terminal.Wheel.followsThePageOnDisplayAndTheMouseProtocol", "[terminal]")
{
    // What a wheel notch does, by what the application sent before it. An application that enabled mouse
    // tracking has the wheel reported, as Windows Terminal asks for tracking before alternate scroll. Else
    // page 1's history scrolls -- the frontend scrolls it for a notch left unhandled -- whatever ?1007 says,
    // as xterm's alternate scroll applies to its alternate buffer only. Over a page without history the wheel
    // becomes cursor keys while ?1007 is set, as it is from power-on, in DECCKM's form at the notch, as
    // xterm, Windows Terminal and foot send them. With DECPCCM reset, that is the page on display, not the
    // cursor's.
    using namespace vtbackend;
    struct Row
    {
        std::string_view name;
        std::string_view sent;
        Handled handled;
        std::string_view reply;
    };
    auto const rows = std::array {
        Row { .name = "page 1", .sent = "", .handled = Handled { false }, .reply = "" },
        Row { .name = "page 1, ?1007 set", .sent = "\033[?1007h", .handled = Handled { false }, .reply = "" },
        Row {
            .name = "page 1, ?1007 reset", .sent = "\033[?1007l", .handled = Handled { false }, .reply = "" },
        Row { .name = "the alternate screen",
              .sent = "\033[?1049h",
              .handled = Handled { true },
              .reply = "\033[B" },
        Row { .name = "the alternate screen, then DECCKM",
              .sent = "\033[?1049h\033[?1h",
              .handled = Handled { true },
              .reply = "\033OB" },
        Row { .name = "tracking, then the alternate screen",
              .sent = "\033[?1000h\033[?1006h\033[?1049h",
              .handled = Handled { true },
              .reply = "\033[<65;1;1M" },
        Row { .name = "the alternate screen, tracking, then DECCKM",
              .sent = "\033[?1049h\033[?1000h\033[?1006h\033[?1h",
              .handled = Handled { true },
              .reply = "\033[<65;1;1M" },
        Row { .name = "the alternate screen, tracking turned on and off again",
              .sent = "\033[?1049h\033[?1000h\033[?1000l",
              .handled = Handled { true },
              .reply = "\033[B" },
        Row { .name = "DECCKM, then the alternate screen",
              .sent = "\033[?1h\033[?1049h",
              .handled = Handled { true },
              .reply = "\033OB" },
        Row { .name = "the alternate screen, DECCKM, then a mouse protocol reset",
              .sent = "\033[?1049h\033[?1h\033[?1000l",
              .handled = Handled { true },
              .reply = "\033OB" },
        Row { .name = "the alternate screen, DECCKM, then tracking turned on and off again",
              .sent = "\033[?1049h\033[?1h\033[?1000h\033[?1000l",
              .handled = Handled { true },
              .reply = "\033OB" },
        Row { .name = "the alternate screen, ?1007 reset",
              .sent = "\033[?1049h\033[?1007l",
              .handled = Handled { false },
              .reply = "" },
        Row { .name = "the alternate screen, ?1007 reset, then set",
              .sent = "\033[?1049h\033[?1007l\033[?1007h",
              .handled = Handled { true },
              .reply = "\033[B" },
        Row { .name = "DECPCCM reset, page 1 on display, the cursor on page 3",
              .sent = "\033[?64l\033[3 P",
              .handled = Handled { false },
              .reply = "" },
        Row { .name = "DECPCCM reset, page 3 on display, the cursor on page 1",
              .sent = "\033[3 P\033[?64l\033[1 P",
              .handled = Handled { true },
              .reply = "\033[B" },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
        mock.terminal.tick(1s);
        mock.writeToScreen(row.sent);
        mock.terminal.tick(1s);

        mock.resetReplyData();
        auto const handled = mock.terminal.sendMousePressEvent(Modifier::None,
                                                               MouseButton::WheelDown,
                                                               mock.terminal.currentMousePosition(),
                                                               PixelCoordinate {},
                                                               false);
        CHECK(handled == row.handled);
        CHECK(e(mock.replyData()) == e(row.reply));
    }
}

TEST_CASE("Terminal.Wheel.alternateScrollIsOnFromPowerOnAndAfterRIS", "[terminal]")
{
    // ?1007, alternate scroll's gate, is set at power-on and by RIS, as Windows Terminal and foot start with
    // it on: DECRQM reports it set.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto const reportedState = [&] {
        mock.resetReplyData();
        mock.writeToScreen("\033[?1007$p"); // DECRQM
        mock.terminal.flushInput();
        return mock.replyData();
    };

    CHECK(e(reportedState()) == e("\033[?1007;1$y"));
    mock.writeToScreen("\033[?1007l");
    CHECK(e(reportedState()) == e("\033[?1007;2$y"));
    mock.writeToScreen("\033c"); // RIS
    CHECK(e(reportedState()) == e("\033[?1007;1$y"));
}

TEST_CASE("Terminal.followOutput.brings_the_view_down_as_the_listener_and_the_settings_ask", "[terminal]")
{
    // New output brings a view scrolled back into the history down to it, under the lock it is written under,
    // where the listener follows output, auto_scroll_on_update is set and the view is in Insert mode -- and
    // only output that added lines: what is repainted in place on the page is not new output.
    using vtbackend::OutputFollowing;
    using vtbackend::ViMode;

    // Appends a line: the cursor sits on the page's last row, so the line feed scrolls the page.
    constexpr auto AppendedLine = std::string_view { "more\r\n" };

    // A prompt repainting itself in place, as a shell's line editor does on a timer: up two rows, each
    // rewritten across line feeds that never reach the bottom margin, and back to where it started. Shaped
    // after the endo shell's idle frame, which also changes a digit of its own (a seconds counter).
    constexpr auto RepaintedInPlace =
        std::string_view { "\033[?2026h\033[2A\033[?25l\033[K\n\033[3C9\033[K\n\033[K\033[?25h\033[?2026l" };

    // A line fed into the history by a scroll region anchored at the top, with a status row kept below it,
    // as a progress bar pinned to the bottom of the page does (apt's): only the region scrolls.
    constexpr auto TopAnchoredRegionLine = std::string_view { "\033[1;4r\033[4;1Hmore\n" };

    // ED 2 scrolls the whole page into the history rather than erasing it.
    constexpr auto ClearedPage = std::string_view { "\033[2J" };

    struct Row
    {
        std::string_view name;
        OutputFollowing following;
        bool autoScrollOnUpdate;
        ViMode mode;
        std::string_view output;
        OutputFollowing outcome;
    };
    auto const rows = std::array {
        Row { .name = "a listener that follows",
              .following = OutputFollowing::Follow,
              .autoScrollOnUpdate = true,
              .mode = ViMode::Insert,
              .output = AppendedLine,
              .outcome = OutputFollowing::Follow },
        Row { .name = "a listener that stays",
              .following = OutputFollowing::Stay,
              .autoScrollOnUpdate = true,
              .mode = ViMode::Insert,
              .output = AppendedLine,
              .outcome = OutputFollowing::Stay },
        Row { .name = "auto_scroll_on_update off",
              .following = OutputFollowing::Follow,
              .autoScrollOnUpdate = false,
              .mode = ViMode::Insert,
              .output = AppendedLine,
              .outcome = OutputFollowing::Stay },
        Row { .name = "Vi Normal mode",
              .following = OutputFollowing::Follow,
              .autoScrollOnUpdate = true,
              .mode = ViMode::Normal,
              .output = AppendedLine,
              .outcome = OutputFollowing::Stay },
        Row { .name = "a prompt repainting itself in place",
              .following = OutputFollowing::Follow,
              .autoScrollOnUpdate = true,
              .mode = ViMode::Insert,
              .output = RepaintedInPlace,
              .outcome = OutputFollowing::Stay },
        Row { .name = "a top-anchored scroll region feeding the history",
              .following = OutputFollowing::Follow,
              .autoScrollOnUpdate = true,
              .mode = ViMode::Insert,
              .output = TopAnchoredRegionLine,
              .outcome = OutputFollowing::Follow },
        Row { .name = "a clear, which scrolls the page into the history",
              .following = OutputFollowing::Follow,
              .autoScrollOnUpdate = true,
              .mode = ViMode::Insert,
              .output = ClearedPage,
              .outcome = OutputFollowing::Follow },
    };

    for (auto const& row: rows)
    {
        INFO(row.name);
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
        mc.following = row.following;
        mc.terminal.settings().autoScrollOnUpdate = row.autoScrollOnUpdate;
        printLines(mc, "line", 20);
        mc.terminal.inputHandler().setMode(row.mode);
        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));

        mc.writeToScreen(row.output);

        auto const outcome =
            mc.terminal.viewport().scrolled() ? OutputFollowing::Stay : OutputFollowing::Follow;
        CHECK(outcome == row.outcome);
    }
}

TEST_CASE("Terminal.smoothLinefeed.shows_the_view_brought_down_to_the_output", "[terminal]")
{
    // A linefeed under smooth scrolling (DECSCLM) shows the output so far before its batch ends, and so
    // before the batch's end brings the view down: it brings the view down itself first, or the lines it
    // already added would be shown to a view still scrolled back.
    using vtbackend::OutputFollowing;
    struct ShowingTerm final: MockTerm<>
    {
        using MockTerm<>::MockTerm;

        void screenUpdated() override
        {
            shown.push_back(terminal.viewport().scrolled() ? OutputFollowing::Stay : OutputFollowing::Follow);
        }

        std::vector<OutputFollowing> shown;
    };

    auto mc = ShowingTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    mc.following = OutputFollowing::Follow;
    mc.terminal.settings().autoScrollOnUpdate = true;
    mc.terminal.settings().smoothLineScrolling = 1ms;
    printLines(mc, "line", 20);
    mc.writeToScreen("\033[?4h");
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
    mc.shown.clear();

    mc.writeToScreen("\n\n");

    // Each linefeed is shown before it scrolls. The first one, then, is shown before the batch has added
    // anything to follow; the second one to a view brought down to the line the first one added.
    REQUIRE(mc.shown.size() >= 2);
    CHECK(mc.shown[0] == OutputFollowing::Stay);
    CHECK(mc.shown[1] == OutputFollowing::Follow);
}

TEST_CASE("Terminal.smoothLinefeed.what_the_rest_of_its_batch_sends_on_the_spot_goes_out", "[terminal]")
{
    // A linefeed under smooth scrolling (DECSCLM) shows the output so far before the next line: the parser
    // thread lets go of the state lock for the pause, and takes it back as its own. Taken back as a
    // frontend's, the hold kept back what the rest of the batch sends on the spot -- here the report a
    // colour palette query asks for (DSR 996).
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.terminal.settings().smoothLineScrolling = 1ms;
    mc.writeToScreen("\033[?4h");
    mc.discardPendingReplies();

    mc.writeToScreen("\n\033[?996n");

    CHECK(mc.replyData().starts_with("\033[?997;"));
}

TEST_CASE("Terminal.inputMethod.composing_brings_the_view_down", "[terminal]")
{
    // Composing is typing, and the text being composed is drawn at the cursor: a view scrolled back into the
    // history comes down to it, as it does for a key. Ending a composition types nothing.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    printLines(mc, "line", 20);
    mc.terminal.updateInputMethodPreeditString("k");
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));

    mc.terminal.updateInputMethodPreeditString("");
    CHECK(mc.terminal.viewport().scrolled());

    mc.terminal.updateInputMethodPreeditString("ka");
    CHECK_FALSE(mc.terminal.viewport().scrolled());
}

TEST_CASE("Terminal.wheelGlide.moves_the_view_without_racing_the_parser_thread", "[terminal][threading]")
{
    // A glide moves the view each frame, on the render thread, as that ticks. The parser thread moves a
    // scrolled-back view too, under the state lock: output keeps it on the lines it shows. The case to run
    // under ThreadSanitizer, which reports the glide's moves against the parser's unless the glide takes that
    // lock as well.
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 1000 } };
    auto& terminal = mc.terminal;
    for ([[maybe_unused]] auto const line: std::views::iota(0, 100))
        mc.writeToScreen("line\r\n");
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    auto now = chrono::steady_clock::time_point();
    terminal.tick(now);
    auto glides = 0;
    {
        auto const writer = writeOnOwnThread([&] { mc.writeToScreen("x\r\n"); });
        for ([[maybe_unused]] auto const frame: std::views::iota(0, 200))
        {
            if (!terminal.isMomentumScrollActive())
            {
                auto const _ = std::scoped_lock { terminal };
                if (terminal.injectWheelMomentum(180.0f, now) == SmoothScrollResult::Applied)
                    ++glides;
            }
            now += 16ms;
            terminal.tick(now);
        }
    }

    CHECK(glides > 1);
    CHECK(terminal.viewport().scrolled());
}

TEST_CASE("Terminal.wheelGlide.is_not_stepped_where_a_synchronized_update_ends", "[terminal]")
{
    // A glide steps on the render thread's tick alone. Where a synchronized update ends, the parser thread
    // sets the terminal's clock to the time it builds that frame at, and moves no glide on.
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 40 } };
    auto& terminal = mc.terminal;
    for ([[maybe_unused]] auto const line: std::views::iota(0, 30))
        mc.writeToScreen("line\r\n");
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
    auto constexpr CellHeight = 20.0f;

    auto const start = mc.steadyClock.now();
    terminal.tick(start);
    REQUIRE(terminal.injectWheelMomentum(180.0f, start) == SmoothScrollResult::Applied);

    mc.steadyClock.advance(50ms);
    mc.writeToScreen("\033[?2026h\033[?2026l");
    CHECK(terminal.currentTime() == start + 50ms);
    CHECK(totalScrollPixels(terminal, CellHeight) == 0.0f);

    terminal.tick(start + 50ms);
    CHECK(totalScrollPixels(terminal, CellHeight) > 0.0f);
}

TEST_CASE("Terminal.animations.the_clock_they_run_on_does_not_race_the_parser_thread",
          "[terminal][threading]")
{
    // The render thread ticks the terminal's clock before and after each frame, and asks when the next frame
    // is due: sooner while the cursor glides to where it moved, or the screens cross-fade. The parser thread
    // builds a frame itself where a synchronized update ends, which starts the cursor's glide, and a switch
    // of screens starts the cross-fade. The case to run under ThreadSanitizer, which reports the parser
    // thread's frame and switch against the render thread's tick and question unless each piece of that state
    // has one owner or is read race-free.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
    auto& terminal = mc.terminal;
    terminal.settings().screenTransitionStyle = vtbackend::ScreenTransitionStyle::Fade;
    terminal.settings().screenTransitionDuration = 50ms;
    REQUIRE(terminal.settings().cursorMotionAnimationDuration.count() > 0);

    {
        auto const writer = writeOnOwnThread([&] {
            mc.writeToScreen("\033[?2026h\033[5;5H\033[?2026l");
            mc.writeToScreen("\033[?1049h\033[?1049l");
            mc.writeToScreen("\033[?2026h\033[1;1H\033[?2026l");
        });
        for ([[maybe_unused]] auto const frame: std::views::iota(0, 200))
        {
            mc.steadyClock.advance(16ms);
            terminal.tick(mc.steadyClock.now());
            (void) terminal.nextRender();
            std::this_thread::sleep_for(1ms);
        }
    }

    CHECK_FALSE(terminal.isAlternateScreen());
}

TEST_CASE("Terminal.frames are scheduled without racing what schedules them", "[terminal][threading]")
{
    // The render thread ticks the terminal and asks when the next frame is due, without the state lock: when
    // the cursor blinks next, from its display (DECSCUSR) and its blink interval; the blinking cells, from
    // their blink style; the indicator status line's clock (DECSSDT); and the rate the frames come at, from
    // when the last one came. The parser thread changes some of them under that lock, the frontend the
    // others. The case to run under ThreadSanitizer, which reports each change against the render thread's
    // read unless that read is race-free.
    struct Row
    {
        std::string_view setting;
        std::function<void(MockTerm<>&)> change;
    };
    auto const rows = std::array {
        Row { .setting = "the cursor's display (DECSCUSR)",
              .change = [](auto& mc) { mc.writeToScreen("\033[2 q\033[1 q"); } },
        Row { .setting = "the cursor's blink interval",
              .change =
                  [](auto& mc) {
                      auto const _ = std::scoped_lock { mc.terminal };
                      mc.terminal.setCursorBlinkingInterval(
                          mc.terminal.cursorBlinkInterval() == 500ms ? 400ms : 500ms);
                  } },
        Row { .setting = "the cells' blink style",
              .change =
                  [](auto& mc) {
                      auto const _ = std::scoped_lock { mc.terminal };
                      auto& style = mc.terminal.settings().blinkStyle;
                      style = style == vtbackend::BlinkStyle::Classic ? vtbackend::BlinkStyle::Smooth
                                                                      : vtbackend::BlinkStyle::Classic;
                  } },
        Row { .setting = "the status line (DECSSDT)",
              .change = [](auto& mc) { mc.writeToScreen("\033[1$~\033[0$~"); } },
        Row { .setting = "the refresh rate",
              .change =
                  [](auto& mc) {
                      mc.terminal.setRefreshRate(vtbackend::RefreshRate { .value = 60 });
                      mc.terminal.setRefreshRate(vtbackend::RefreshRate { .value = 120 });
                  } },
    };

    for (auto const& row: rows)
    {
        INFO(row.setting);
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
        mc.writeToScreen("\033[1 q\033[5mblinking\033[m");
        auto const frames = runWhileWriting([&] { row.change(mc); },
                                            [&] {
                                                mc.steadyClock.advance(16ms);
                                                mc.terminal.tick(mc.steadyClock.now());
                                                (void) mc.terminal.refreshRenderBuffer();
                                                (void) mc.terminal.nextRender();
                                            });
        CHECK(frames > 0);
    }
}

TEST_CASE("Terminal.smoothScroll.the_pixel_offset_is_read_without_racing_a_scroll", "[terminal][threading]")
{
    // The renderer reads the smooth-scroll pixel offset as it draws the frame it just filled, and the GUI
    // thread as it maps the pointer onto a cell, neither under the state lock, while a scroll moves it under
    // that lock: the wheel's, here, or a glide's. The case to run under ThreadSanitizer, which reports the
    // reads against the moves unless the offset is read race-free.
    auto mc = MockTerm { PageSize { LineCount { 4 }, ColumnCount { 10 } }, LineCount { 100 } };
    auto& terminal = mc.terminal;
    for ([[maybe_unused]] auto const line: std::views::iota(0, 30))
        mc.writeToScreen("line\r\n");
    terminal.setCellPixelSize(vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });

    auto strayOffsets = 0;
    (void) runWhileWriting(
        [&] {
            auto const _ = std::scoped_lock { terminal };
            (void) terminal.applySmoothScrollPixelDelta(5.0f);
            (void) terminal.applySmoothScrollPixelDelta(-5.0f);
        },
        [&] {
            (void) terminal.refreshRenderBuffer();
            auto const offset = terminal.smoothScrollPixelOffset();
            if (offset != 0.0f && offset != 5.0f)
                ++strayOffsets;
        });

    CHECK(strayOffsets == 0);
}

TEST_CASE("Terminal.the layout a frame is drawn at is read without racing its writers",
          "[terminal][threading]")
{
    // The renderer reads the page's layout each frame without the state lock: the main page's size, the
    // status line's height and position, the row the main page starts at, and whether the primary screen is
    // shown. The parser thread changes some of them under that lock -- switching screens, putting the status
    // line up -- and the frontend the others: it resizes, and a reload moves the status line. The case to run
    // under ThreadSanitizer, which reports each change against the renderer's read unless that read is
    // race-free.
    struct Row
    {
        std::string_view change;
        std::function<void(MockTerm<>&)> write;
    };
    auto const rows = std::array {
        Row { .change = "the alternate screen (DECSET 1049)",
              .write = [](auto& mc) { mc.writeToScreen("\033[?1049h\033[?1049l"); } },
        Row { .change = "the status line (DECSSDT)",
              .write = [](auto& mc) { mc.writeToScreen("\033[1$~\033[0$~"); } },
        Row { .change = "a resize, with the status line up",
              .write =
                  [](auto& mc) {
                      auto const _ = std::scoped_lock { mc.terminal };
                      auto const columns = mc.terminal.totalPageSize().columns == ColumnCount(40) ? 30 : 40;
                      mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(columns) });
                  } },
        Row { .change = "the status line's position (a reload)",
              .write =
                  [](auto& mc) {
                      auto const _ = std::scoped_lock { mc.terminal };
                      auto& position = mc.terminal.settings().statusDisplayPosition;
                      position = position == vtbackend::StatusDisplayPosition::Top
                                     ? vtbackend::StatusDisplayPosition::Bottom
                                     : vtbackend::StatusDisplayPosition::Top;
                  } },
    };

    for (auto const& row: rows)
    {
        INFO(row.change);
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
        mc.writeToScreen("\033[1$~");
        // Each read on its own: a frame's reads are not one snapshot, so only what holds for each is checked.
        auto strayLayouts = size_t { 0 };
        auto framesAtTop = size_t { 0 };
        auto framesOnAlternate = size_t { 0 };
        auto const frames = runWhileWriting(
            [&] { row.write(mc); },
            [&] {
                auto& terminal = mc.terminal;
                (void) terminal.refreshRenderBuffer();
                if (!terminal.isPrimaryScreen())
                    ++framesOnAlternate;
                if (terminal.settings().statusDisplayPosition == vtbackend::StatusDisplayPosition::Top)
                    ++framesAtTop;
                if (terminal.mainPageSize().lines < LineCount(9) || terminal.statusLineHeight() > LineCount(1)
                    || terminal.mainPageTopRow() > LineOffset(1))
                    ++strayLayouts;
            });
        CHECK(strayLayouts == 0);
        CHECK(framesAtTop <= frames);
        CHECK(framesOnAlternate <= frames);
    }
}

// NOLINTEND(misc-const-correctness)
