// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/App.hpp>
#include <crispy/Times.hpp>
#include <crispy/Utils.hpp>
#include <crispy/testing/Environment.hpp>

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
#include <mutex>
#include <ranges>
#include <string>
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

    // Move cursor (start first animation).
    mc.writeToScreen("A");
    terminal.tick(ClockBase + 200ms);
    terminal.ensureFreshRenderBuffer();

    auto fromAfterFirst = std::optional<vtbackend::CellLocation> {};
    {
        auto const buf1 = terminal.renderBuffer();
        REQUIRE(buf1.get().cursor.has_value());
        fromAfterFirst = buf1.get().cursor->animateFrom;
        REQUIRE(fromAfterFirst.has_value());
    } // Release RenderBufferRef lock before next render cycle.

    // Tick partway through animation (40ms into default 80ms).
    terminal.tick(ClockBase + 240ms);

    // Chain: move cursor again while animation is still in progress.
    mc.writeToScreen("B");
    terminal.tick(ClockBase + 300ms);
    terminal.ensureFreshRenderBuffer();

    auto const buf2 = terminal.renderBuffer();
    REQUIRE(buf2.get().cursor.has_value());
    auto const& cursor = *buf2.get().cursor;

    // The new animateFrom should be an interpolated position (not the original from-position).
    CHECK(cursor.animateFrom.has_value());
    CHECK(cursor.animationProgress < 1.0f);
    // The chained from-position should differ from the first animation's from-position,
    // because it was computed from the interpolated mid-animation point.
    CHECK(cursor.animateFrom != fromAfterFirst);
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
    // DECSET ?1007 (alternate-scroll) was previously a no-op because of the mouse-protocol
    // gate; it must now produce cursor keys on the alternate screen.
    using namespace vtbackend;
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(10) };
    auto constexpr NoPixel = PixelCoordinate {};
    mock.terminal.tick(1s);

    mock.writeToScreen("\033[?1049h"); // alt screen
    mock.writeToScreen("\033[?1007h"); // alternate-scroll -> application cursor keys
    mock.terminal.tick(1s);

    mock.resetReplyData();
    CHECK(mock.terminal.sendMousePressEvent(
              Modifier::None, MouseButton::WheelDown, mock.terminal.currentMousePosition(), NoPixel, false)
          == Handled { true });
    CHECK(e(mock.replyData()) == e("\033OB"));
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

// NOLINTEND(misc-const-correctness)
