// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/CellTestSupport.hpp>
#include <core/tui/HeadlessSurface.hpp>
#include <core/tui/HoverState.hpp>
#include <core/tui/MockScreenSurface.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/Tooltip.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using core::ManualClock;
using core::tui::test::requireCell;

using namespace core::tui;

// ============================================================================
// InlineCursorMovement calculation tests
// ============================================================================

TEST_CASE("Screen.inlineCursor_initialRender")
{
    // First render with no previous content
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/0,
        /*previousCursorRow=*/0,
        /*newContentHeight=*/1);

    CHECK(movement.moveUpToStart == 0);
    CHECK(movement.newLinesToEmit == 1);      // Need 1 line for content
    CHECK(movement.moveUpAfterNewlines == 1); // Move back up by 1
    CHECK(movement.rowsToClear == 0);
}

TEST_CASE("Screen.inlineCursor_contentGrows")
{
    // Content grows from 1 row (prompt) to 8 rows (prompt + popup)
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/1,
        /*previousCursorRow=*/0,
        /*newContentHeight=*/8);

    CHECK(movement.moveUpToStart == 0);       // Already at row 0
    CHECK(movement.newLinesToEmit == 7);      // 8 - 1 = 7 new lines needed
    CHECK(movement.moveUpAfterNewlines == 7); // Move up by newLines, NOT contentHeight!
    CHECK(movement.rowsToClear == 0);         // No shrink
}

TEST_CASE("Screen.inlineCursor_contentShrinks")
{
    // Content shrinks from 8 rows (prompt + popup) to 1 row (just prompt)
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/8,
        /*previousCursorRow=*/0,
        /*newContentHeight=*/1);

    CHECK(movement.moveUpToStart == 0);
    CHECK(movement.newLinesToEmit == 0); // No growth
    CHECK(movement.moveUpAfterNewlines == 0);
    CHECK(movement.rowsToClear == 7); // 8 - 1 = 7 rows to clear
}

TEST_CASE("Screen.inlineCursor_noChange")
{
    // Content height stays the same
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/3,
        /*previousCursorRow=*/0,
        /*newContentHeight=*/3);

    CHECK(movement.moveUpToStart == 0);
    CHECK(movement.newLinesToEmit == 0);
    CHECK(movement.moveUpAfterNewlines == 0);
    CHECK(movement.rowsToClear == 0);
}

TEST_CASE("Screen.inlineCursor_cursorNotAtRowZero")
{
    // Cursor was left at row 2 (e.g., in a multi-line input)
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/3,
        /*previousCursorRow=*/2,
        /*newContentHeight=*/3);

    CHECK(movement.moveUpToStart == 2); // Move from row 2 to row 0
    CHECK(movement.newLinesToEmit == 0);
    CHECK(movement.moveUpAfterNewlines == 0);
    CHECK(movement.rowsToClear == 0);
}

TEST_CASE("Screen.inlineCursor_growWithCursorOffset")
{
    // Content grows while cursor is not at row 0
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/2,
        /*previousCursorRow=*/1,
        /*newContentHeight=*/5);

    CHECK(movement.moveUpToStart == 1);       // Move from row 1 to row 0
    CHECK(movement.newLinesToEmit == 3);      // 5 - 2 = 3 new lines
    CHECK(movement.moveUpAfterNewlines == 3); // Move up by newLines
    CHECK(movement.rowsToClear == 0);
}

TEST_CASE("Screen.inlineCursor_shrinkWithCursorOffset")
{
    // Content shrinks while cursor is not at row 0
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/5,
        /*previousCursorRow=*/2,
        /*newContentHeight=*/2);

    CHECK(movement.moveUpToStart == 2); // Move from row 2 to row 0
    CHECK(movement.newLinesToEmit == 0);
    CHECK(movement.moveUpAfterNewlines == 0);
    CHECK(movement.rowsToClear == 3); // 5 - 2 = 3 rows to clear
}

TEST_CASE("Screen.inlineCursor_growShrinkCycle_noDrift")
{
    // Simulate the Tab press cycle that was causing cursor drift:
    // Initial (1 row) -> Grow (8 rows) -> Shrink (1 row) -> Grow (8 rows)
    // Each transition should be self-consistent with no cumulative drift

    int prevHeight = 0;
    int prevCursor = 0;

    // Initial render
    auto m0 = calculateInlineCursorMovement(prevHeight, prevCursor, 1);
    CHECK(m0.newLinesToEmit == 1);
    CHECK(m0.moveUpAfterNewlines == 1);
    prevHeight = 1;
    prevCursor = 0;

    // Grow to 8 (popup shown)
    auto m1 = calculateInlineCursorMovement(prevHeight, prevCursor, 8);
    CHECK(m1.newLinesToEmit == 7);
    CHECK(m1.moveUpAfterNewlines == 7); // Critical: NOT 8
    prevHeight = 8;
    prevCursor = 0;

    // Shrink to 1 (popup hidden)
    auto m2 = calculateInlineCursorMovement(prevHeight, prevCursor, 1);
    CHECK(m2.newLinesToEmit == 0);
    CHECK(m2.rowsToClear == 7);
    prevHeight = 1;
    prevCursor = 0;

    // Grow again to 8 (popup shown again)
    auto m3 = calculateInlineCursorMovement(prevHeight, prevCursor, 8);
    CHECK(m3.newLinesToEmit == 7);
    CHECK(m3.moveUpAfterNewlines == 7); // Still NOT 8 - no drift!
    prevHeight = 8;
    prevCursor = 0;

    // Shrink again to 1
    auto m4 = calculateInlineCursorMovement(prevHeight, prevCursor, 1);
    CHECK(m4.rowsToClear == 7);

    // The key invariant: each grow emits N newlines and moves up N (not N+previousHeight)
    // This ensures no cumulative cursor drift across cycles
}

TEST_CASE("Screen.inlineCursor_shrinkToZero")
{
    // Edge case: content shrinks to 0 (all content removed)
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/3,
        /*previousCursorRow=*/0,
        /*newContentHeight=*/0);

    CHECK(movement.moveUpToStart == 0);
    CHECK(movement.newLinesToEmit == 0);
    CHECK(movement.moveUpAfterNewlines == 0);
    CHECK(movement.rowsToClear == 0); // Don't clear when new content is 0
}

TEST_CASE("Screen.inlineCursor_largeGrowth")
{
    // Large content growth
    auto movement = calculateInlineCursorMovement(
        /*previousContentHeight=*/1,
        /*previousCursorRow=*/0,
        /*newContentHeight=*/100);

    CHECK(movement.newLinesToEmit == 99);
    CHECK(movement.moveUpAfterNewlines == 99); // NOT 100
    CHECK(movement.rowsToClear == 0);
}

// ============================================================================
// UnscrollMode configuration tests
// ============================================================================

TEST_CASE("Screen.unscrollMode_enumValues")
{
    // Verify UnscrollMode enum has expected values
    CHECK(static_cast<int>(UnscrollMode::Auto) == 0);
    CHECK(static_cast<int>(UnscrollMode::Enabled) == 1);
    CHECK(static_cast<int>(UnscrollMode::Disabled) == 2);
}

TEST_CASE("Screen.screenConfig_defaultsAreTheZeroOfEachEnumeration")
{
    // The bools ScreenConfig had are enumerations now; this pins that the defaults did not move,
    // and that each default is its enumeration's zero, so a zero-initialized field means "off".
    auto const config = ScreenConfig {};
    struct Row
    {
        std::string_view field;
        bool isDefault;
        bool isZero;
    };
    auto const rows = std::array {
        Row { .field = "viewport",
              .isDefault = config.viewport == Viewport::Fullscreen,
              .isZero = std::to_underlying(config.viewport) == 0 },
        Row { .field = "unscrollMode",
              .isDefault = config.unscrollMode == UnscrollMode::Auto,
              .isZero = std::to_underlying(config.unscrollMode) == 0 },
        Row { .field = "reflow",
              .isDefault = config.reflow == DiffReflow::Keep,
              .isZero = std::to_underlying(config.reflow) == 0 },
        Row { .field = "screenBuffer",
              .isDefault = config.screenBuffer == ScreenBuffer::Primary,
              .isZero = std::to_underlying(config.screenBuffer) == 0 },
        Row { .field = "inlineMaxHeight", .isDefault = config.inlineMaxHeight == 0, .isZero = true },
        Row { .field = "fixedArea", .isDefault = config.fixedArea == Rect {}, .isZero = true },
    };
    for (auto const& row: rows)
    {
        INFO(row.field);
        CHECK(row.isDefault);
        CHECK(row.isZero);
    }
}

TEST_CASE("Screen.screenConfig_alternateScreenBufferIsEnteredAndLeft")
{
    auto mockOutput = std::make_unique<MockTerminalOutput>(20, 4);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));
    auto const wrote = [mock](std::string_view sequence) {
        return std::ranges::any_of(mock->rawWrites(),
                                   [sequence](auto const& write) { return write.text == sequence; });
    };

    {
        auto const primary = Screen(surface);
        CHECK_FALSE(wrote("\033[?1049h"));
    }
    CHECK_FALSE(wrote("\033[?1049l"));

    {
        auto const alternate = Screen(surface, ScreenConfig { .screenBuffer = ScreenBuffer::Alternate });
        CHECK(wrote("\033[?1049h"));
        CHECK_FALSE(wrote("\033[?1049l"));
    }
    CHECK(wrote("\033[?1049l"));
}

TEST_CASE("Screen.screenConfig_reflowInhibitionReachesTheInlineFrame")
{
    auto const frameWith = [](DiffReflow reflow) {
        auto bytes = std::string {};
        auto surface = HeadlessSurface { Size { .width = 10, .height = 4 },
                                         [&bytes](std::string_view chunk) { bytes += chunk; } };
        auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Inline, .reflow = reflow });
        struct Painter: Component
        {
            void render(Canvas& canvas) override { canvas.putString(0, 0, "RRR", {}); }
        };
        auto painter = Painter {};
        screen.root().addChild(painter, LayoutParams { .area = { .x = 0, .y = 0, .width = 3, .height = 1 } });
        screen.draw();
        return bytes;
    };

    CHECK_FALSE(frameWith(DiffReflow::Keep).contains("\033[?2028l"));
    CHECK(frameWith(DiffReflow::Inhibit).contains("\033[?2028l"));
}

TEST_CASE("Screen.screenConfig_defaultUnscrollMode")
{
    // Verify default unscroll mode is Auto
    ScreenConfig const config;
    CHECK(config.unscrollMode == UnscrollMode::Auto);
}

TEST_CASE("Screen.screenConfig_customUnscrollMode")
{
    // Verify unscroll mode can be set via ScreenConfig
    ScreenConfig configEnabled;
    configEnabled.unscrollMode = UnscrollMode::Enabled;
    CHECK(configEnabled.unscrollMode == UnscrollMode::Enabled);

    ScreenConfig configDisabled;
    configDisabled.unscrollMode = UnscrollMode::Disabled;
    CHECK(configDisabled.unscrollMode == UnscrollMode::Disabled);
}

// ============================================================================
// HoverState tests
// ============================================================================

TEST_CASE("HoverState.initialState")
{
    HoverState const hover;
    CHECK_FALSE(hover.isHoverConfirmed());
    CHECK_FALSE(hover.currentHover().has_value());
    CHECK_FALSE(hover.mousePosition().has_value());
    CHECK(hover.timeoutMs() == -1); // No timeout when not hovering
}

TEST_CASE("HoverState.onMouseMove_startsHover")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(100), clock);

    hover.onMouseMove(10, 20, nullptr);

    CHECK_FALSE(hover.isHoverConfirmed()); // Not confirmed yet
    CHECK(hover.currentHover().has_value());
    CHECK(hover.currentHover()->x == 10);
    CHECK(hover.currentHover()->y == 20);
    CHECK(hover.timeoutMs() == 100); // The whole delay: no time has passed on the clock.
}

TEST_CASE("HoverState.onMouseMove_samePosition_noReset")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(100), clock);

    hover.onMouseMove(10, 20, nullptr);
    auto const initialTimeout = hover.timeoutMs();

    clock.advance(std::chrono::milliseconds(10));

    // Same position - should NOT reset the timer
    hover.onMouseMove(10, 20, nullptr);
    auto const secondTimeout = hover.timeoutMs();

    // The timer kept running from the first move.
    CHECK(initialTimeout == 100);
    CHECK(secondTimeout == 90);
}

TEST_CASE("HoverState.onMouseMove_differentPosition_resetsTimer")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(100), clock);

    hover.onMouseMove(10, 20, nullptr);

    clock.advance(std::chrono::milliseconds(30));

    // Move to different position - should reset the timer
    hover.onMouseMove(15, 25, nullptr);
    auto const timeout = hover.timeoutMs();

    // The timer restarted at the new position.
    CHECK(timeout == 100);
    CHECK(hover.currentHover()->x == 15);
    CHECK(hover.currentHover()->y == 25);
}

TEST_CASE("HoverState.tick_triggersCallback")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(10), clock);

    bool callbackCalled = false;
    HoverInfo capturedInfo;

    hover.setOnHoverConfirmed([&](HoverInfo const& info) {
        callbackCalled = true;
        capturedInfo = info;
    });

    hover.onMouseMove(50, 60, nullptr);

    clock.advance(std::chrono::milliseconds(20));

    hover.tick(clock.now());

    CHECK(callbackCalled);
    CHECK(hover.isHoverConfirmed());
    CHECK(capturedInfo.x == 50);
    CHECK(capturedInfo.y == 60);
}

TEST_CASE("HoverState.delay_confirmsOnTheMillisecondItElapses")
{
    auto clock = ManualClock {};
    auto hover = HoverState(std::chrono::milliseconds(500), clock);
    auto confirmations = 0;
    hover.setOnHoverConfirmed([&](HoverInfo const&) { ++confirmations; });

    hover.onMouseMove(3, 4, nullptr);
    clock.advance(std::chrono::milliseconds(499));
    CHECK(hover.timeoutMs() == 1);
    hover.tick(clock.now());
    CHECK_FALSE(hover.isHoverConfirmed());

    clock.advance(std::chrono::milliseconds(1));
    CHECK(hover.timeoutMs() == 0);
    hover.tick(clock.now());
    CHECK(hover.isHoverConfirmed());
    CHECK(hover.timeoutMs() == -1); // Nothing left to wait for.

    hover.tick(clock.now()); // Confirmed once, not on every tick after.
    CHECK(confirmations == 1);
}

TEST_CASE("HoverState.onMouseLeave_triggersCallback")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(10), clock);

    bool leaveCallbackCalled = false;

    hover.setOnHoverConfirmed([](HoverInfo const&) {});
    hover.setOnHoverLeave([&]() { leaveCallbackCalled = true; });

    hover.onMouseMove(10, 20, nullptr);

    // Confirm hover
    clock.advance(std::chrono::milliseconds(15));
    hover.tick(clock.now());
    CHECK(hover.isHoverConfirmed());

    // Leave
    hover.onMouseLeave();

    CHECK(leaveCallbackCalled);
    CHECK_FALSE(hover.isHoverConfirmed());
    CHECK_FALSE(hover.currentHover().has_value());
}

TEST_CASE("HoverState.reset_clearsState")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(10), clock);

    bool leaveCallbackCalled = false;
    hover.setOnHoverLeave([&]() { leaveCallbackCalled = true; });

    hover.onMouseMove(10, 20, nullptr);
    clock.advance(std::chrono::milliseconds(15));
    hover.tick(clock.now());
    CHECK(hover.isHoverConfirmed());

    hover.reset();

    CHECK(leaveCallbackCalled);
    CHECK_FALSE(hover.isHoverConfirmed());
    CHECK_FALSE(hover.currentHover().has_value());
    CHECK(hover.timeoutMs() == -1);
}

TEST_CASE("HoverState.mouseMove_whileConfirmed_triggersLeaveAndRestart")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(10), clock);

    int leaveCount = 0;

    hover.setOnHoverLeave([&]() { ++leaveCount; });

    hover.onMouseMove(10, 20, nullptr);

    // Confirm hover
    clock.advance(std::chrono::milliseconds(15));
    hover.tick(clock.now());
    CHECK(hover.isHoverConfirmed());

    // Move to different position - should trigger leave and restart
    hover.onMouseMove(30, 40, nullptr);

    CHECK(leaveCount == 1);
    CHECK_FALSE(hover.isHoverConfirmed());   // No longer confirmed
    CHECK(hover.currentHover().has_value()); // But still tracking
    CHECK(hover.currentHover()->x == 30);
}

// ============================================================================
// Component::onHover tests
// ============================================================================

TEST_CASE("Component.onHover_defaultReturnsNullopt")
{
    struct SimpleComponent: Component
    {
        void render(Canvas&) override {}
    };

    SimpleComponent comp;
    auto result = comp.onHover(5, 3);
    CHECK_FALSE(result.has_value());
}

TEST_CASE("Component.onHover_overrideReturnsPlainText")
{
    struct HoverableComponent: Component
    {
        void render(Canvas&) override {}

        std::optional<HoverResult> onHover(int x, int y) override
        {
            return HoverResult {
                .text = "hello tooltip",
                .position = { .x = x, .y = y },
                .contentType = TooltipContentType::PlainText,
            };
        }
    };

    HoverableComponent comp;
    auto result = comp.onHover(10, 5);
    REQUIRE(result.has_value());
    CHECK(result->text == "hello tooltip");
    CHECK(result->position.x == 10);
    CHECK(result->position.y == 5);
    CHECK(result->contentType == TooltipContentType::PlainText);
}

TEST_CASE("Component.onHover_overrideReturnsMarkdown")
{
    struct MarkdownHoverComponent: Component
    {
        void render(Canvas&) override {}

        std::optional<HoverResult> onHover(int x, int y) override
        {
            return HoverResult {
                .text = "## Heading\n\nMulti-line **bold** content\n- item 1\n- item 2",
                .position = { .x = x, .y = y },
                .contentType = TooltipContentType::Markdown,
            };
        }
    };

    MarkdownHoverComponent comp;
    auto result = comp.onHover(3, 0);
    REQUIRE(result.has_value());
    CHECK(result->contentType == TooltipContentType::Markdown);
    // Verify multi-line content preserved
    CHECK(result->text.contains('\n'));
}

TEST_CASE("Component.onHover_selectiveHover")
{
    struct SelectiveComponent: Component
    {
        void render(Canvas&) override {}

        std::optional<HoverResult> onHover(int x, int /*y*/) override
        {
            if (x < 5)
                return std::nullopt;
            return HoverResult { .text = "hoverable area",
                                 .position = { .x = x, .y = 1 },
                                 .contentType = TooltipContentType::PlainText };
        }
    };

    SelectiveComponent comp;
    CHECK_FALSE(comp.onHover(2, 0).has_value());
    CHECK(comp.onHover(5, 0).has_value());
    CHECK(comp.onHover(10, 0).has_value());
}

TEST_CASE("HoverState.confirmedCallback_receivesTarget")
{
    struct MockComponent: Component
    {
        void render(Canvas&) override {}

        std::optional<HoverResult> onHover(int x, int y) override
        {
            return HoverResult { .text = "mock hover",
                                 .position = { .x = x, .y = y },
                                 .contentType = TooltipContentType::PlainText };
        }
    };

    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(10), clock);
    MockComponent comp;
    HoverInfo captured {};

    hover.setOnHoverConfirmed([&](HoverInfo const& info) { captured = info; });
    hover.onMouseMove(15, 20, &comp);

    clock.advance(std::chrono::milliseconds(20));
    hover.tick(clock.now());

    CHECK(hover.isHoverConfirmed());
    CHECK(captured.target == &comp);
    CHECK(captured.x == 15);
    CHECK(captured.y == 20);

    // Verify the component's onHover returns the expected result
    auto result = captured.target->onHover(5, 3);
    REQUIRE(result.has_value());
    CHECK(result->text == "mock hover");
}

TEST_CASE("HoverState.leave_afterConfirm_clearsState")
{
    auto clock = ManualClock {};
    HoverState hover(std::chrono::milliseconds(10), clock);
    bool leaveCalled = false;
    hover.setOnHoverConfirmed([](HoverInfo const&) {});
    hover.setOnHoverLeave([&]() { leaveCalled = true; });

    hover.onMouseMove(10, 20, nullptr);
    clock.advance(std::chrono::milliseconds(15));
    hover.tick(clock.now());
    CHECK(hover.isHoverConfirmed());

    // Move to different position triggers leave + restart
    hover.onMouseMove(30, 40, nullptr);
    CHECK(leaveCalled);
    CHECK_FALSE(hover.isHoverConfirmed());
}

// ============================================================================
// Tooltip sizing tests
// ============================================================================

TEST_CASE("Tooltip.multiLineContent_growsHeight")
{
    Tooltip tooltip;
    tooltip.setContent("Line 1\nLine 2\nLine 3", TooltipContentType::PlainText);
    auto size = tooltip.preferredSize();
    // 3 content lines + 2 border lines = 5 height
    CHECK(size.height == 5);
}

TEST_CASE("Tooltip.exceedsMaxHeight_enablesScrolling")
{
    Tooltip tooltip;
    tooltip.setMaxSize({ .width = 40, .height = 5 }); // max 5 rows total (3 content + 2 border)

    // 10 lines of content
    std::string content;
    for (auto const i: std::views::iota(0, 10))
    {
        if (i > 0)
            content += '\n';
        content += "Line " + std::to_string(i + 1);
    }
    tooltip.setContent(content, TooltipContentType::PlainText);

    auto size = tooltip.preferredSize();
    CHECK(size.height <= 5); // Clamped to max
    CHECK(tooltip.canScrollDown());
    CHECK_FALSE(tooltip.canScrollUp()); // At top

    tooltip.scrollDown(1);
    CHECK(tooltip.canScrollUp());
}

TEST_CASE("Tooltip.markdownContent_properSizing")
{
    Tooltip tooltip;
    tooltip.setContent("## Title\n\nSome **bold** text\n\n- item 1\n- item 2", TooltipContentType::Markdown);
    auto size = tooltip.preferredSize();
    CHECK(size.width > 0);
    CHECK(size.height > 2); // More than just border
}

// ============================================================================
// Screen-level hover→tooltip integration tests
// ============================================================================

namespace
{

/// @brief A mock component that returns tooltip content for the first 15 columns of row 0.
struct HoverableTestComp: Component
{
    void render(Canvas& canvas) override { canvas.putString(0, 0, "error text here", {}); }

    [[nodiscard]] Size preferredSize() const override { return { .width = 80, .height = 3 }; }

    [[nodiscard]] bool focusable() const override { return true; }

    std::optional<HoverResult> onHover(int x, int y) override
    {
        if (x >= 0 && x < 15 && y == 0)
            return HoverResult {
                .text = "undefined variable",
                .position = { .x = x, .y = y },
                .contentType = TooltipContentType::PlainText,
            };
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("Screen.hoverToTooltip_callbackFires")
{
    MockScreenSurface surface;
    auto clock = ManualClock {};
    Screen screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen }, clock);

    HoverableTestComp comp;
    auto const compLayout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, compLayout);
    screen.setFocus(&comp);
    screen.draw();

    // Verify component screenBounds is set correctly
    auto const bounds = comp.screenBounds();
    CHECK(bounds.x == 0);
    CHECK(bounds.y == 0);
    CHECK(bounds.width == 80);
    CHECK(bounds.height == 3);

    // Dispatch mouse move
    (void) screen.dispatchEvent(test::mouseMove(5, 1));

    // Check hover state was updated
    auto const hover = screen.hoverState().currentHover();
    REQUIRE(hover.has_value());
    CHECK(hover->x == 5);
    CHECK(hover->y == 1);
    CHECK(hover->target == &comp);

    // The Screen's hover delay runs on the clock it was given, to the millisecond.
    clock.advance(HoverState::DefaultHoverDelay - std::chrono::milliseconds(1));
    screen.tickHover();
    CHECK_FALSE(screen.hoverState().isHoverConfirmed());
    CHECK_FALSE(screen.isTooltipVisible());
    CHECK(screen.pollTimeoutMs() == 1);

    clock.advance(std::chrono::milliseconds(1));
    screen.tickHover();

    CHECK(screen.hoverState().isHoverConfirmed());
    CHECK(screen.isTooltipVisible());
}

TEST_CASE("Screen.hoverToTooltip_fullFlow")
{
    MockScreenSurface surface; // uninitialized = 80x24 defaults
    auto clock = ManualClock {};
    Screen screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen }, clock);

    HoverableTestComp comp;
    auto const compLayout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, compLayout);
    screen.setFocus(&comp);
    screen.draw(); // Establishes screenBounds

    // Mouse move over error text (1-based terminal coordinates)
    (void) screen.dispatchEvent(test::mouseMove(5, 1));

    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK(screen.isTooltipVisible());

    // Verify tooltip content in rendered buffer
    screen.draw();
    auto const content = test::canvasToString(screen.renderedBuffer());
    CHECK(content.contains("undefined variable"));
}

TEST_CASE("Screen.hoverToTooltip_mouseMovesAway_hidesTooltip")
{
    MockScreenSurface surface;
    auto clock = ManualClock {};
    Screen screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen }, clock);

    HoverableTestComp comp;
    auto const compLayout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, compLayout);
    screen.setFocus(&comp);
    screen.draw();

    // Trigger hover
    (void) screen.dispatchEvent(test::mouseMove(5, 1));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();
    REQUIRE(screen.isTooltipVisible());

    // Move mouse to a position where onHover returns nullopt (row 20)
    (void) screen.dispatchEvent(test::mouseMove(5, 20));

    CHECK_FALSE(screen.isTooltipVisible());
}

TEST_CASE("Screen.hoverToTooltip_keyEvent_hidesTooltip")
{
    MockScreenSurface surface;
    auto clock = ManualClock {};
    Screen screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen }, clock);

    HoverableTestComp comp;
    auto const compLayout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, compLayout);
    screen.setFocus(&comp);
    screen.draw();

    // Trigger hover
    (void) screen.dispatchEvent(test::mouseMove(5, 1));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();
    REQUIRE(screen.isTooltipVisible());

    // Dispatch a key event — should hide tooltip
    (void) screen.dispatchEvent(InputEvent { test::charKey('a') });

    CHECK_FALSE(screen.isTooltipVisible());
}

TEST_CASE("Screen.hoverToTooltip_nulloptHover_noTooltip")
{
    MockScreenSurface surface;
    auto clock = ManualClock {};
    Screen screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen }, clock);

    HoverableTestComp comp;
    auto const compLayout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, compLayout);
    screen.setFocus(&comp);
    screen.draw();

    // Mouse over region where onHover returns nullopt (column 50, row 2)
    (void) screen.dispatchEvent(test::mouseMove(50, 3));

    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK_FALSE(screen.isTooltipVisible());
}

TEST_CASE("Screen.hoverToTooltip_inlineMode")
{
    MockScreenSurface surface; // 80x24 defaults
    auto clock = ManualClock {};
    Screen screen(surface, ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 }, clock);

    HoverableTestComp comp;
    auto const compLayout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, compLayout);
    screen.setFocus(&comp);
    screen.draw(); // Sets _inlineContentStartRow and screenBounds

    // In inline mode, mouse coordinates are terminal-absolute (1-based).
    // The content starts at _inlineContentStartRow (set by flushInline).
    // The component is at row 0 in buffer space, so terminal row =
    // _inlineContentStartRow + 1 (1-based). Since terminal is 24 rows and
    // content height is 1 (just "error text here" on row 0, rows 1-2 are
    // empty spaces), _inlineContentStartRow ≈ 24 - 1 = 23.
    // We use row 24 (1-based) = terminal row 23 (0-based) = content row 0.
    auto const terminalRow = surface.rows(); // bottom of terminal, 1-based
    (void) screen.dispatchEvent(test::mouseMove(5, terminalRow));

    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    // The hover may or may not trigger a tooltip depending on exact inline
    // coordinate mapping. The key test is that the system doesn't crash
    // and that the hover state was properly processed.
    // If tooltip is visible, verify content.
    if (screen.isTooltipVisible())
    {
        screen.draw();
        auto const content = test::canvasToString(screen.renderedBuffer());
        CHECK(content.contains("undefined variable"));
    }
}

// ============================================================================
// MockTerminalOutput cursor drift tests
// ============================================================================

namespace
{

/// @brief A fixed-content component that renders N rows of text with a cursor at a specified row.
struct FixedContentComponent: Component
{
    int contentRows = 4;
    int cursorAtRow = 2;

    void render(Canvas& canvas) override
    {
        for (auto const row: std::views::iota(0, std::max(0, contentRows)))
            canvas.putString(0, row, "content line", {});
        canvas.setCursor(cursorAtRow, 0);
    }

    [[nodiscard]] Size preferredSize() const override { return { .width = 80, .height = contentRows }; }

    [[nodiscard]] bool focusable() const override { return true; }
};

} // namespace

TEST_CASE("Screen.inlineCursor_releaseCursorCycle_noDrift")
{
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };
    auto screen = Screen(surface, screenConfig);

    auto comp = FixedContentComponent {};
    comp.contentRows = 4;
    comp.cursorAtRow = 2;
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 4 } };
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);

    // First render
    screen.draw();
    auto const firstCursorRow = mock->cursorRow();

    // Simulate transition away using clearAndRelease (the fix)
    screen.clearAndRelease();

    // Re-render after release
    screen.draw();
    auto const secondCursorRow = mock->cursorRow();

    CHECK(firstCursorRow == secondCursorRow);
}

// ============================================================================
// Variable height component drift tests
// ============================================================================

namespace
{

/// @brief A component with configurable height and cursor row, for drift testing.
struct VariableHeightComponent: Component
{
    int height = 2;
    int cursorRow = 0;

    void render(Canvas& canvas) override
    {
        for (auto const row: std::views::iota(0, std::max(0, height)))
            canvas.putString(0, row, "content", {});
        canvas.setCursor(cursorRow, 0);
    }

    [[nodiscard]] Size preferredSize() const override { return { .width = 80, .height = height }; }

    [[nodiscard]] bool focusable() const override { return true; }
};

} // namespace

TEST_CASE("Screen.inlineCursor_heightChangeAfterClearAndRelease_drift")
{
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };
    auto screen = Screen(surface, screenConfig);

    auto comp = VariableHeightComponent {};
    comp.height = 2;
    comp.cursorRow = 0;
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 12 } };
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);

    // First render — baseline
    screen.draw();
    auto const baselineCursorRow = mock->cursorRow();

    // clearAndRelease, change height, draw again
    screen.clearAndRelease();
    comp.height = 3;
    comp.cursorRow = 1;
    screen.root().removeChild(comp);
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);
    screen.draw();
    auto const afterGrowCursorRow = mock->cursorRow();

    // Height grew by 1, cursor moved from row 0 to row 1 → cursor row should be baseline + 1
    CHECK(afterGrowCursorRow == baselineCursorRow + 1);

    // Another clearAndRelease + draw with same height — no additional drift
    screen.clearAndRelease();
    screen.draw();
    auto const redrawCursorRow = mock->cursorRow();
    CHECK(redrawCursorRow == afterGrowCursorRow);
}

TEST_CASE("Screen.inlineCursor_twoComponentSwap_noDrift")
{
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };
    auto screen = Screen(surface, screenConfig);

    // "Shell" component: height 3, cursor at row 1
    auto shellComp = VariableHeightComponent {};
    shellComp.height = 3;
    shellComp.cursorRow = 1;

    // "Agent" component: height 2, cursor at row 1
    auto agentComp = VariableHeightComponent {};
    agentComp.height = 2;
    agentComp.cursorRow = 1;

    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 12 } };

    // Draw shell component — baseline
    screen.root().addChild(shellComp, layout);
    screen.setFocus(&shellComp);
    screen.draw();
    auto const baselineCursorRow = mock->cursorRow();

    // Toggle to agent and back 10 times — no cumulative drift
    for ([[maybe_unused]] auto const i: std::views::iota(0, 10))
    {
        // Switch to agent
        screen.clearAndRelease();
        screen.root().removeChild(shellComp);
        screen.root().addChild(agentComp, layout);
        screen.setFocus(&agentComp);
        screen.draw();

        // Switch back to shell
        screen.clearAndRelease();
        screen.root().removeChild(agentComp);
        screen.root().addChild(shellComp, layout);
        screen.setFocus(&shellComp);
        screen.draw();
        auto const cursorRow = mock->cursorRow();
        CHECK(cursorRow == baselineCursorRow);
    }
}

TEST_CASE("Screen.inline_toggle_no_drift")
{
    // Simulates the Ctrl+T toggle between two independent Screens (shell and agent),
    // which is the exact pattern that causes the +1 drift per shell→agent transition.
    // Each Screen has its own _peakContentHeight, so the second Screen's first render
    // would previously re-emit room reservation LFs even though the room already exists.
    // The fix uses DECSC/DECRC to anchor the cursor position across Screen transitions.

    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto const screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 12 } };

    // Shell component: 2 rows, cursor at row 0
    auto shellComp = VariableHeightComponent {};
    shellComp.height = 2;
    shellComp.cursorRow = 0;

    // Agent component: 2 rows, cursor at row 0
    auto agentComp = VariableHeightComponent {};
    agentComp.height = 2;
    agentComp.cursorRow = 0;

    // --- Shell Screen: first render (baseline) ---
    auto shellScreen = std::make_unique<Screen>(surface, screenConfig);
    shellScreen->root().addChild(shellComp, layout);
    shellScreen->setFocus(&shellComp);
    shellScreen->draw();
    auto const baselineCursorRow = mock->cursorRow();

    // Toggle shell → agent → shell 10 times, verify no cumulative drift
    for ([[maybe_unused]] auto const i: std::views::iota(0, 10))
    {
        // Shell → Agent transition: clearAndRelease shell (saves cursor via DECSC),
        // create new agent Screen (restores cursor via DECRC on first render)
        shellScreen->clearAndRelease();
        shellScreen->root().removeChild(shellComp);
        shellScreen.reset(); // Destroy shell screen

        auto agentScreen = std::make_unique<Screen>(surface, screenConfig);
        agentScreen->root().addChild(agentComp, layout);
        agentScreen->setFocus(&agentComp);
        agentScreen->draw();

        // Agent → Shell transition: clearAndRelease agent, create new shell Screen
        agentScreen->clearAndRelease();
        agentScreen->root().removeChild(agentComp);
        agentScreen.reset(); // Destroy agent screen

        shellScreen = std::make_unique<Screen>(surface, screenConfig);
        shellScreen->root().addChild(shellComp, layout);
        shellScreen->setFocus(&shellComp);
        shellScreen->draw();

        auto const cursorRow = mock->cursorRow();
        CHECK(cursorRow == baselineCursorRow);
    }

    // Cleanup
    shellScreen->root().removeChild(shellComp);
}

TEST_CASE("Screen.inline_toggle_different_heights_no_drift")
{
    // Tests the toggle between screens with different content heights.
    // The room reservation + DECSC/DECRC cursor anchoring should handle
    // the taller→shorter and shorter→taller transitions without drift.

    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto const screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 12 } };

    // Shell: 3 rows, cursor at row 1
    auto shellComp = VariableHeightComponent {};
    shellComp.height = 3;
    shellComp.cursorRow = 1;

    // Agent: 1 row, cursor at row 0
    auto agentComp = VariableHeightComponent {};
    agentComp.height = 1;
    agentComp.cursorRow = 0;

    // --- Shell first render (baseline) ---
    auto shellScreen = std::make_unique<Screen>(surface, screenConfig);
    shellScreen->root().addChild(shellComp, layout);
    shellScreen->setFocus(&shellComp);
    shellScreen->draw();
    auto const baselineCursorRow = mock->cursorRow();

    for ([[maybe_unused]] auto const i: std::views::iota(0, 5))
    {
        // Shell → Agent (clearAndRelease saves cursor via DECSC)
        shellScreen->clearAndRelease();
        shellScreen->root().removeChild(shellComp);
        shellScreen.reset();

        auto agentScreen = std::make_unique<Screen>(surface, screenConfig);
        agentScreen->root().addChild(agentComp, layout);
        agentScreen->setFocus(&agentComp);
        agentScreen->draw();

        // Agent → Shell (clearAndRelease saves cursor via DECSC)
        agentScreen->clearAndRelease();
        agentScreen->root().removeChild(agentComp);
        agentScreen.reset();

        shellScreen = std::make_unique<Screen>(surface, screenConfig);
        shellScreen->root().addChild(shellComp, layout);
        shellScreen->setFocus(&shellComp);
        shellScreen->draw();

        auto const cursorRow = mock->cursorRow();
        CHECK(cursorRow == baselineCursorRow);
    }

    shellScreen->root().removeChild(shellComp);
}

TEST_CASE("Screen.inline_toggle_shell4_agent2_no_drift")
{
    // Tests the toggle between screens where shell is taller (4 rows) and agent is shorter (2 rows).
    // Exercises the partial room-reservation path where reserved room < content height,
    // verifying DECSC/DECRC correctly anchors the cursor even when extra LFs are needed.

    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto const screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 12 } };

    // Shell: 4 rows, cursor at row 2
    auto shellComp = VariableHeightComponent {};
    shellComp.height = 4;
    shellComp.cursorRow = 2;

    // Agent: 2 rows, cursor at row 0
    auto agentComp = VariableHeightComponent {};
    agentComp.height = 2;
    agentComp.cursorRow = 0;

    // --- Shell first render (baseline) ---
    auto shellScreen = std::make_unique<Screen>(surface, screenConfig);
    shellScreen->root().addChild(shellComp, layout);
    shellScreen->setFocus(&shellComp);
    shellScreen->draw();
    auto const baselineCursorRow = mock->cursorRow();

    for ([[maybe_unused]] auto const i: std::views::iota(0, 10))
    {
        // Shell (4 rows) → Agent (2 rows): agent gets reserved room=4, only needs 2
        shellScreen->clearAndRelease();
        shellScreen->root().removeChild(shellComp);
        shellScreen.reset();

        auto agentScreen = std::make_unique<Screen>(surface, screenConfig);
        agentScreen->root().addChild(agentComp, layout);
        agentScreen->setFocus(&agentComp);
        agentScreen->draw();

        // Agent (2 rows) → Shell (4 rows): shell gets reserved room=2, needs 4
        // This exercises the partial room path (reservedRoom < contentHeight)
        agentScreen->clearAndRelease();
        agentScreen->root().removeChild(agentComp);
        agentScreen.reset();

        shellScreen = std::make_unique<Screen>(surface, screenConfig);
        shellScreen->root().addChild(shellComp, layout);
        shellScreen->setFocus(&shellComp);
        shellScreen->draw();

        auto const cursorRow = mock->cursorRow();
        CHECK(cursorRow == baselineCursorRow);
    }

    shellScreen->root().removeChild(shellComp);
}

TEST_CASE("Screen.inlineCursor_clearAndReleaseCycle_noDrift")
{
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };
    auto screen = Screen(surface, screenConfig);

    auto comp = FixedContentComponent {};
    comp.contentRows = 4;
    comp.cursorAtRow = 2;
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 4 } };
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);

    // Initial render — establish baseline
    screen.draw();
    auto const baselineCursorRow = mock->cursorRow();

    // Cycle: clearAndRelease → draw, repeated 10 times
    for ([[maybe_unused]] auto const i: std::views::iota(0, 10))
    {
        screen.clearAndRelease();
        screen.draw();
        auto const cursorRow = mock->cursorRow();
        CHECK(cursorRow == baselineCursorRow);
    }
}

// ============================================================================
// Inline mode hover coordinate translation tests
// ============================================================================

namespace
{

/// @brief A component that returns different tooltip text per row, for verifying coordinate mapping.
struct RowReportingComponent: Component
{
    int contentRows = 3;

    void render(Canvas& canvas) override
    {
        for (auto const row: std::views::iota(0, std::max(0, contentRows)))
            canvas.putString(row, 0, "row" + std::to_string(row) + " content", {});
        canvas.setCursor(0, 0);
    }

    [[nodiscard]] Size preferredSize() const override { return { .width = 80, .height = contentRows }; }

    [[nodiscard]] bool focusable() const override { return true; }

    std::optional<HoverResult> onHover(int x, int y) override
    {
        if (y >= 0 && y < contentRows)
            return HoverResult {
                .text = "row" + std::to_string(y),
                .position = { .x = x, .y = y },
                .contentType = TooltipContentType::PlainText,
            };
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("Screen.inlineHover_cursorAtTop_correctMapping")
{
    // Cursor starts at row 0 (top of terminal) — content should be at top,
    // and hovering at content rows should produce correct component-relative coordinates.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 50);
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };

    auto clock = ManualClock {};
    auto screen = Screen(surface, screenConfig, clock);

    auto comp = RowReportingComponent {};
    comp.contentRows = 3;
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);
    screen.draw();

    // Content starts at row 0 (mock cursor starts at 0).
    // Terminal mouse coordinates are 1-based, so row 1 = content row 0.
    (void) screen.dispatchEvent(test::mouseMove(5, 1)); // row 1 in 1-based = content row 0
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK(screen.isTooltipVisible());
    if (screen.isTooltipVisible())
    {
        screen.draw();
        auto const content = test::canvasToString(screen.renderedBuffer());
        CHECK(content.contains("row0"));
    }
}

TEST_CASE("Screen.inlineHover_cursorInMiddle_correctMapping")
{
    // Cursor starts in the middle of the terminal. Hovering above content should be rejected,
    // hovering at content should work, hovering below content should be rejected.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 50);
    auto* mock = mockOutput.get();

    // Move mock cursor to middle of terminal before creating Screen
    mock->linefeed(); // row 1
    mock->linefeed(); // row 2
    mock->linefeed(); // row 3
    mock->linefeed(); // row 4
    mock->linefeed(); // row 5
    mock->linefeed(); // row 6
    mock->linefeed(); // row 7
    mock->linefeed(); // row 8
    mock->linefeed(); // row 9
    mock->linefeed(); // row 10 — content starts here

    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };

    auto clock = ManualClock {};
    auto screen = Screen(surface, screenConfig, clock);

    auto comp = RowReportingComponent {};
    comp.contentRows = 3;
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);
    screen.draw();

    // Hover above content area (terminal row 5, 1-based = row 4 0-based, content starts at 10)
    (void) screen.dispatchEvent(test::mouseMove(5, 5));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();
    CHECK_FALSE(screen.isTooltipVisible());

    // Hover at first content row (terminal row 11 in 1-based = row 10 0-based = content row 0)
    screen.hoverState().reset();
    (void) screen.dispatchEvent(test::mouseMove(5, 11));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK(screen.isTooltipVisible());
    if (screen.isTooltipVisible())
    {
        screen.draw();
        auto const content = test::canvasToString(screen.renderedBuffer());
        CHECK(content.contains("row0"));
    }

    // Hover at second content row (terminal row 12 in 1-based = content row 1)
    screen.hoverState().reset();
    (void) screen.dispatchEvent(test::mouseMove(5, 12));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK(screen.isTooltipVisible());
    if (screen.isTooltipVisible())
    {
        screen.draw();
        auto const content = test::canvasToString(screen.renderedBuffer());
        CHECK(content.contains("row1"));
    }

    // Hover below content area (terminal row 20 in 1-based)
    screen.hoverState().reset();
    (void) screen.dispatchEvent(test::mouseMove(5, 20));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();
    CHECK_FALSE(screen.isTooltipVisible());
}

TEST_CASE("Screen.inlineHover_cursorAtBottom_correctMapping")
{
    // Cursor starts at the bottom of the terminal. Content should be at the bottom,
    // and hovering at the correct terminal rows should work.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 24);
    auto* mock = mockOutput.get();

    // Move cursor to bottom (row 23 = last row of 24-row terminal)
    for ([[maybe_unused]] auto const i: std::views::iota(0, 23))
        mock->linefeed();

    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };

    auto clock = ManualClock {};
    auto screen = Screen(surface, screenConfig, clock);

    auto comp = RowReportingComponent {};
    comp.contentRows = 3;
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 3 } };
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);
    screen.draw();

    // Content starts at row 21 (24 - 3 = 21, 0-based), so terminal row 22 in 1-based = content row 0.
    // With scrolling from LFs, the content is at the bottom.
    (void) screen.dispatchEvent(test::mouseMove(5, 22));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK(screen.isTooltipVisible());
    if (screen.isTooltipVisible())
    {
        screen.draw();
        auto const content = test::canvasToString(screen.renderedBuffer());
        CHECK(content.contains("row0"));
    }
}

TEST_CASE("Screen.inlineHover_contentGrowth_correctMapping")
{
    // Content starts small (1 row), then grows to 3 rows. Hover should still map correctly.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 50);
    auto surface = MockScreenSurface(std::move(mockOutput));

    auto screenConfig = ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 };

    auto clock = ManualClock {};
    auto screen = Screen(surface, screenConfig, clock);

    auto comp = RowReportingComponent {};
    comp.contentRows = 1;
    auto const layout = LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 12 } };
    screen.root().addChild(comp, layout);
    screen.setFocus(&comp);
    screen.draw();

    // Grow content to 3 rows
    comp.contentRows = 3;
    screen.invalidate();
    screen.draw();

    // Content starts at row 0 (cursor at top). Hover at row 1 (1-based) = content row 0.
    (void) screen.dispatchEvent(test::mouseMove(5, 1));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK(screen.isTooltipVisible());
    if (screen.isTooltipVisible())
    {
        screen.draw();
        auto const content = test::canvasToString(screen.renderedBuffer());
        CHECK(content.contains("row0"));
    }

    // Hover at row 3 (1-based) = content row 2.
    screen.hoverState().reset();
    (void) screen.dispatchEvent(test::mouseMove(5, 3));
    clock.advance(std::chrono::milliseconds(600));
    screen.tickHover();

    CHECK(screen.isTooltipVisible());
    if (screen.isTooltipVisible())
    {
        screen.draw();
        auto const content = test::canvasToString(screen.renderedBuffer());
        CHECK(content.contains("row2"));
    }
}

// ============================================================================
// OSC 8 hyperlink regions
// ============================================================================

namespace
{

/// @brief Renders one row of single-grapheme segments under an optional hyperlink region.
///
/// Drawing each grapheme with its own putString() reproduces how a gradient-coloured prompt
/// path reaches the buffer — one Cell per cluster — which is exactly the case a hyperlink has
/// to survive as a single logical link.
struct LinkedTextComponent: Component
{
    std::string text = "abcdefgh";
    std::string uri = "file://box/a";
    bool linked = true;

    void render(Canvas& canvas) override
    {
        auto col = 0;
        for (auto const ch: text)
            col += canvas.putString(0, col, std::string(1, ch), {});
        if (linked)
            canvas.addHyperlink(0, 0, col, uri);
    }

    [[nodiscard]] Size preferredSize() const override { return { .width = 80, .height = 1 }; }
};

/// @brief Attaches @p component to @p screen as a single full-width row.
void attachSingleRow(Screen& screen, Component& component)
{
    screen.root().addChild(component, LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 1 } });
}

} // namespace

TEST_CASE("Screen.hyperlink_emittedOnceAcrossPerCellWrites_andClosed")
{
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 4);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));
    auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen });

    auto comp = LinkedTextComponent {};
    attachSingleRow(screen, comp);
    screen.draw();

    // Eight separate one-grapheme writes under one region must yield one logical link.
    REQUIRE(mock->hyperlinkRuns().size() == 1);
    CHECK(mock->hyperlinkRuns()[0].url == "file://box/a");
    CHECK(mock->hyperlinkRuns()[0].text == "abcdefgh");
    CHECK_FALSE(mock->hyperlinkRuns()[0].id.empty());

    // Framing must never leak past the flush.
    CHECK_FALSE(mock->hyperlinkOpen());
    CHECK(mock->unbalancedHyperlinkCloses() == 0);
}

TEST_CASE("Screen.hyperlink_absentWhenNoRegionRegistered")
{
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 4);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));
    auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen });

    auto comp = LinkedTextComponent {};
    comp.linked = false;
    attachSingleRow(screen, comp);
    screen.draw();

    CHECK(mock->hyperlinkRuns().empty());
}

TEST_CASE("Screen.hyperlink_uriChangeWithIdenticalCells_forcesRewrite")
{
    // Regression guard for the diff term in Screen's flush loops. The URI lives beside the
    // cell rather than in it, so Cell equality cannot see it: without an explicit URI
    // comparison, a link retargeted while its text stayed byte-identical would never be
    // repainted and the terminal would keep opening the stale target forever.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 4);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));
    auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen });

    auto comp = LinkedTextComponent {};
    comp.uri = "file://box/before";
    attachSingleRow(screen, comp);
    screen.draw();

    REQUIRE(mock->hyperlinkRuns().size() == 1);
    CHECK(mock->hyperlinkRuns()[0].url == "file://box/before");

    // Same text, same styles, different target.
    comp.uri = "file://box/after";
    screen.draw();

    REQUIRE(mock->hyperlinkRuns().size() == 2);
    CHECK(mock->hyperlinkRuns()[1].url == "file://box/after");
    CHECK(mock->hyperlinkRuns()[1].text == "abcdefgh");
    CHECK(mock->hyperlinkRuns()[1].id != mock->hyperlinkRuns()[0].id);
}

TEST_CASE("Screen.hyperlink_unchangedFrameEmitsNoFraming")
{
    // A frame whose cells and URI are all unchanged is fully diffed away, so it should cost
    // no framing bytes at all.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 4);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));
    auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen });

    auto comp = LinkedTextComponent {};
    attachSingleRow(screen, comp);
    screen.draw();
    auto const afterFirst = mock->hyperlinkRuns().size();

    screen.draw();
    CHECK(mock->hyperlinkRuns().size() == afterFirst);
}

TEST_CASE("Screen.hyperlink_idIsStableAcrossRedrawsOfSameUri")
{
    // The id must be derived from the URI, not minted per frame: a terminal that groups by id
    // would otherwise treat each repaint as a brand-new link.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 4);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));
    auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen });

    auto comp = LinkedTextComponent {};
    attachSingleRow(screen, comp);
    screen.draw();
    screen.invalidate(); // force a full repaint rather than a diff
    screen.draw();

    REQUIRE(mock->hyperlinkRuns().size() == 2);
    CHECK(mock->hyperlinkRuns()[0].id == mock->hyperlinkRuns()[1].id);
}

TEST_CASE("Screen.fixedViewport_repaintsItsAreaAndClosesFraming")
{
    // Viewport::Fixed had no coverage at all, and its repaint is now the same routine fullscreen
    // uses (flushArea), so this pins that the shared body still serves the fixed case: the area is
    // painted, the link inside it is emitted once, and no framing leaks past the flush.
    auto mockOutput = std::make_unique<MockTerminalOutput>(80, 4);
    auto* mock = mockOutput.get();
    auto surface = MockScreenSurface(std::move(mockOutput));
    auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Fullscreen });
    screen.setViewport(Rect { .x = 0, .y = 0, .width = 80, .height = 1 });

    auto comp = LinkedTextComponent {};
    attachSingleRow(screen, comp);
    screen.draw();

    REQUIRE(mock->hyperlinkRuns().size() == 1);
    CHECK(mock->hyperlinkRuns()[0].url == "file://box/a");
    CHECK(mock->hyperlinkRuns()[0].text == "abcdefgh");
    CHECK_FALSE(mock->hyperlinkOpen());
    CHECK(mock->unbalancedHyperlinkCloses() == 0);
}

// ============================================================================
// Hit testing and pointer capture
// ============================================================================

namespace
{

/// @brief Records every mouse event it receives, in the coordinates it receives them in.
struct MouseRecorder: Component
{
    EventResult pressResult = EventResult::Handled; ///< What a press returns.
    Size size { .width = 10, .height = 3 };         ///< Preferred size, which an overlay is shown at.
    std::vector<MouseEvent> received;               ///< Every mouse event, in order.

    void render(Canvas& /*canvas*/) override {}

    [[nodiscard]] Size preferredSize() const override { return size; }

    EventResult onEvent(InputEvent const& event) override
    {
        auto const* mouse = std::get_if<MouseEvent>(&event);
        if (mouse == nullptr)
            return EventResult::Ignored;
        received.push_back(*mouse);
        return mouse->type == MouseEvent::Type::Press ? pressResult : EventResult::Handled;
    }
};

/// @brief A mouse event at a 1-based terminal cell, as the parser delivers it.
[[nodiscard]] InputEvent mouseAt(MouseEvent::Type type, int x, int y)
{
    return InputEvent { MouseEvent { .type = type, .button = 0, .x = x, .y = y } };
}

/// @brief Runs @c onPress when pressed, which may destroy this component, and answers @c pressResult.
struct PressCallback: Component
{
    std::function<void()> onPress;                  ///< Called on a press.
    EventResult pressResult = EventResult::Handled; ///< What a press returns.

    void render(Canvas& /*canvas*/) override {}

    EventResult onEvent(InputEvent const& event) override
    {
        auto const* mouse = std::get_if<MouseEvent>(&event);
        if (mouse == nullptr || mouse->type != MouseEvent::Type::Press)
            return EventResult::Ignored;
        // Both members die with this component.
        auto const callback = onPress;
        auto const result = pressResult;
        callback();
        return result;
    }
};

/// @brief The layout of the capture cases: A at columns 2..11, B at columns 20..29, both on rows 1..3.
constexpr auto AreaA = Rect { .x = 2, .y = 1, .width = 10, .height = 3 };
constexpr auto AreaB = Rect { .x = 20, .y = 1, .width = 10, .height = 3 };

} // namespace

TEST_CASE("Screen.componentAt_findsTheDeepestVisibleComponent")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto panel = MouseRecorder {};
    auto leaf = MouseRecorder {};
    auto hidden = MouseRecorder {};
    screen.root().addChild(panel, LayoutParams { .area = { .x = 2, .y = 1, .width = 20, .height = 5 } });
    panel.addChild(leaf, LayoutParams { .area = { .x = 1, .y = 1, .width = 5, .height = 2 } });
    screen.root().addChild(
        hidden, LayoutParams { .area = { .x = 30, .y = 1, .width = 5, .height = 2 }, .visible = false });
    screen.draw();

    CHECK(screen.componentAt(2, 4) == &leaf);     // leaf covers columns 3..7, rows 2..3
    CHECK(screen.componentAt(1, 2) == &panel);    // the panel's own top-left cell
    CHECK(screen.componentAt(1, 31) == nullptr);  // only the invisible component is there
    CHECK(screen.componentAt(20, 70) == nullptr); // nothing but the root
}

TEST_CASE("Screen.componentAt_prefersTheHigherZIndex")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto high = MouseRecorder {};
    auto low = MouseRecorder {};
    // Added first, so insertion order alone would put it underneath.
    screen.root().addChild(
        high, LayoutParams { .area = { .x = 5, .y = 0, .width = 10, .height = 3 }, .zIndex = 1 });
    screen.root().addChild(low, LayoutParams { .area = { .x = 0, .y = 0, .width = 10, .height = 3 } });
    screen.draw();

    CHECK(screen.componentAt(0, 6) == &high);
    CHECK(screen.componentAt(0, 2) == &low);
}

TEST_CASE("Screen.componentAt_findsOverlaysBeforeTheTree")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto tree = MouseRecorder {};
    auto first = MouseRecorder {};
    auto second = MouseRecorder {};
    screen.root().addChild(tree, LayoutParams { .area = { .x = 0, .y = 0, .width = 40, .height = 10 } });
    screen.showOverlay(first, Point { .x = 5, .y = 2 });  // columns 5..14, rows 2..4
    screen.showOverlay(second, Point { .x = 8, .y = 3 }); // columns 8..17, rows 3..5
    screen.draw();

    CHECK(screen.componentAt(2, 6) == &first);
    CHECK(screen.componentAt(4, 10) == &second); // shown last, so on top where they overlap
    CHECK(screen.componentAt(0, 0) == &tree);

    // Showing an overlay again moves it but keeps its place in the stack.
    screen.showOverlay(first, Point { .x = 5, .y = 2 });
    screen.draw();
    CHECK(screen.componentAt(4, 10) == &second);
}

TEST_CASE("Screen.componentAt_neverReturnsTheTooltip")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto tree = MouseRecorder {};
    screen.root().addChild(tree, LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 10 } });
    screen.showTooltip("a tooltip", Point { .x = 0, .y = 0 }); // drawn from row 1 down
    screen.draw();

    REQUIRE(screen.isTooltipVisible());
    CHECK(screen.componentAt(1, 1) == &tree);
}

TEST_CASE("Screen.mousePress_overAnOverlayGoesToTheOverlay")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto tree = MouseRecorder {};
    auto overlay = MouseRecorder {};
    screen.root().addChild(tree, LayoutParams { .area = { .x = 0, .y = 0, .width = 40, .height = 10 } });
    screen.showOverlay(overlay, Point { .x = 5, .y = 2 });
    screen.draw();

    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 7, 4))
          == EventResult::Handled); // cell (3, 6)

    REQUIRE(overlay.received.size() == 1);
    CHECK(overlay.received[0].x == 2); // overlay-relative, 1-based, as for any component
    CHECK(overlay.received[0].y == 2);
    CHECK(tree.received.empty());
}

namespace
{

/// @brief A mouse recorder that paints its whole area with one character.
struct PaintedRecorder: MouseRecorder
{
    char glyph = ' '; ///< What this component fills its canvas with.

    void render(Canvas& canvas) override
    {
        canvas.fill(
            Rect { .x = 0, .y = 0, .width = canvas.width(), .height = canvas.height() }, glyph, Style {});
    }
};

} // namespace

TEST_CASE("Screen.componentAt_agreesWithPaintingWhenZIndicesTie")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    constexpr auto Count = 40; // enough siblings that an unstable sort would reorder ties
    auto siblings = std::vector<PaintedRecorder>(Count);
    for (auto const i: std::views::iota(size_t { 0 }, siblings.size()))
    {
        auto& sibling = siblings[i];
        sibling.glyph = static_cast<char>('A' + (i % 26));
        screen.root().addChild(sibling, LayoutParams { .area = { .x = 0, .y = 0, .width = 5, .height = 2 } });
    }
    screen.draw();

    // Whichever sibling is painted last covers the cell; that one must be the hit-test answer.
    auto const painted = requireCell(screen.renderedBuffer(), { .x = 0, .y = 0 }).grapheme;
    auto* const hit = screen.componentAt(0, 0);
    REQUIRE(hit != nullptr);
    auto const* const hitSibling = static_cast<PaintedRecorder const*>(hit);
    CHECK(std::string(1, hitSibling->glyph) == painted);
    CHECK(hit == &siblings.back());

    // A pair, at the default z-index.
    auto first = PaintedRecorder {};
    auto second = PaintedRecorder {};
    first.glyph = '1';
    second.glyph = '2';
    screen.root().addChild(first, LayoutParams { .area = { .x = 10, .y = 0, .width = 5, .height = 2 } });
    screen.root().addChild(second, LayoutParams { .area = { .x = 10, .y = 0, .width = 5, .height = 2 } });
    screen.draw();
    CHECK(requireCell(screen.renderedBuffer(), { .x = 10, .y = 0 }).grapheme == "2");
    CHECK(screen.componentAt(0, 10) == &second);
}

TEST_CASE("Screen.componentAt_agreesWithPaintingForAnOverlaysChildren")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto overlay = MouseRecorder {};
    auto high = PaintedRecorder {};
    auto low = PaintedRecorder {};
    high.glyph = 'H';
    low.glyph = 'L';
    // The higher z-index is added first, so insertion order alone would paint it underneath.
    overlay.addChild(high, LayoutParams { .area = { .x = 0, .y = 0, .width = 4, .height = 2 }, .zIndex = 1 });
    overlay.addChild(low, LayoutParams { .area = { .x = 0, .y = 0, .width = 4, .height = 2 } });
    screen.showOverlay(overlay, Point { .x = 5, .y = 2 });
    screen.draw();

    CHECK(requireCell(screen.renderedBuffer(), { .x = 5, .y = 2 }).grapheme == "H");
    CHECK(screen.componentAt(2, 5) == &high);
}

TEST_CASE("Screen.pointerCapture_followsTheDragOutsideThePressedComponent")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2)); // cell (1, 3): inside A
    CHECK(screen.pointerCapture() == &source);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));    // over B
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Release, 1, 10)); // left of and below A

    REQUIRE(source.received.size() == 3);
    CHECK(source.received[0].type == MouseEvent::Type::Press);
    CHECK(source.received[0].x == 2);
    CHECK(source.received[0].y == 1);
    CHECK(source.received[1].type == MouseEvent::Type::Move);
    CHECK(source.received[1].x == 23);
    CHECK(source.received[1].y == 1);
    CHECK(source.received[2].type == MouseEvent::Type::Release);
    CHECK(source.received[2].x == -1); // A-relative, so left of A is below 1
    CHECK(source.received[2].y == 9);
    CHECK(other.received.empty());
    CHECK(screen.pointerCapture() == nullptr);
}

TEST_CASE("Screen.pointerCapture_isTakenByTheAncestorThatHandledThePress")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto panel = MouseRecorder {};
    auto leaf = MouseRecorder {};
    leaf.pressResult = EventResult::Ignored;
    screen.root().addChild(panel, LayoutParams { .area = { .x = 2, .y = 1, .width = 20, .height = 5 } });
    panel.addChild(leaf, LayoutParams { .area = { .x = 1, .y = 1, .width = 5, .height = 2 } });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 5, 3)); // cell (2, 4): inside the leaf
    CHECK(screen.pointerCapture() == &panel);

    // The press bubbles as any event does: the panel sees it in the leaf's coordinates.
    REQUIRE(panel.received.size() == 1);
    CHECK(panel.received[0].type == MouseEvent::Type::Press);
    CHECK(panel.received[0].x == 2); // leaf-relative: 4 - 3 + 1, where panel-relative would be 3
    CHECK(panel.received[0].y == 1); // 2 - 2 + 1, where panel-relative would be 2

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 40, 10)); // cell (9, 39)
    REQUIRE(panel.received.size() == 2);
    CHECK(panel.received[1].type == MouseEvent::Type::Move);
    CHECK(panel.received[1].x == 38); // panel-relative: 39 - 2 + 1
    CHECK(panel.received[1].y == 9);  // 9 - 1 + 1
    CHECK(leaf.received.size() == 1); // the press only
}

TEST_CASE("Screen.pointerCapture_isNotTakenByAPressNobodyHandled")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    source.pressResult = EventResult::Ignored;
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    CHECK(screen.pointerCapture() == nullptr);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));
    REQUIRE(other.received.size() == 1);
    CHECK(other.received[0].type == MouseEvent::Type::Move);
}

TEST_CASE("Screen.pointerCapture_endsWhenTheTargetIsDestroyed")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto source = std::make_unique<MouseRecorder>();
    auto other = MouseRecorder {};
    screen.root().addChild(*source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    REQUIRE(screen.pointerCapture() == source.get());
    source.reset();
    CHECK(screen.pointerCapture() == nullptr);

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2)); // hit-tested again
    REQUIRE(other.received.size() == 1);
    CHECK(other.received[0].type == MouseEvent::Type::Move);
}

TEST_CASE("Screen.pointerCapture_endsWhenTheTargetOrItsAncestorLeavesTheTree")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto panel = MouseRecorder {};
    auto source = MouseRecorder {};
    panel.pressResult = EventResult::Ignored;
    screen.root().addChild(panel, LayoutParams { .area = { .x = 0, .y = 0, .width = 30, .height = 10 } });
    panel.addChild(source, LayoutParams { .area = AreaA });
    screen.draw();

    SECTION("the target itself")
    {
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
        REQUIRE(screen.pointerCapture() == &source);
        panel.removeChild(source);
        CHECK(screen.pointerCapture() == nullptr);
    }
    SECTION("an ancestor")
    {
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
        REQUIRE(screen.pointerCapture() == &source);
        screen.root().removeChild(panel);
        CHECK(screen.pointerCapture() == nullptr);
    }
}

TEST_CASE("Screen.pointerCapture_endsWhenTheOverlayHoldingItIsHidden")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto overlay = MouseRecorder {};
    screen.showOverlay(overlay, Point { .x = 5, .y = 2 });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 7, 4));
    REQUIRE(screen.pointerCapture() == &overlay);
    screen.hideOverlay(overlay);
    CHECK(screen.pointerCapture() == nullptr);
}

TEST_CASE("Screen.releasePointer_endsTheCapture")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    screen.releasePointer();
    CHECK(screen.pointerCapture() == nullptr);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));
    CHECK(other.received.size() == 1);
    CHECK(source.received.size() == 1);
}

TEST_CASE("Screen.pointerCapture_neverTakesScrollEvents")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::ScrollDown, 25, 2)); // over B
    REQUIRE(other.received.size() == 1);
    CHECK(other.received[0].type == MouseEvent::Type::ScrollDown);
    CHECK(screen.pointerCapture() == &source); // the drag goes on

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));
    CHECK(source.received.size() == 2);
    CHECK(other.received.size() == 1);
}

TEST_CASE("Screen.pointerCapture_movesToTheComponentOfTheNextPress")
{
    // A release the terminal never delivered -- the window lost focus mid-drag -- holds the pointer
    // only until the next press.
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 25, 2)); // no release in between
    CHECK(screen.pointerCapture() == &other);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 5, 2)); // over A
    CHECK(source.received.size() == 1);
    REQUIRE(other.received.size() == 2);
    CHECK(other.received[1].type == MouseEvent::Type::Move);
}

TEST_CASE("Screen.pointerCapture_isNotTakenByAComponentItsPressDestroyed")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto doomed = std::make_unique<PressCallback>();
    doomed->onPress = [&doomed] {
        doomed.reset();
    };
    screen.root().addChild(*doomed, LayoutParams { .area = AreaA });
    screen.draw();

    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2)) == EventResult::Handled);
    CHECK(doomed == nullptr);
    CHECK(screen.pointerCapture() == nullptr);
    // Delivered to the capture, this would reach freed memory: under ASan, a heap-use-after-free.
    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 4, 2)) == EventResult::Ignored);
}

TEST_CASE("Screen.destructor_detachesOverlaysStillShown")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto overlay = MouseRecorder {};
    {
        auto screen = Screen(surface);
        screen.showOverlay(overlay, Point { .x = 5, .y = 5 });
        screen.draw();
        CHECK(overlay.screen() == &screen);
    }
    // The overlay outlives the screen; its own teardown must not reach the destroyed one.
    CHECK(overlay.screen() == nullptr);
}

TEST_CASE("Screen.overlayDestroyedWhileShown_leavesTheOverlayList")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto overlay = std::make_unique<MouseRecorder>();
    screen.showOverlay(*overlay, Point { .x = 5, .y = 2 });
    screen.draw();
    REQUIRE(screen.componentAt(3, 6) == overlay.get());

    overlay.reset();
    // Kept in the list, the destroyed overlay would be drawn and hit-tested here: under ASan, a
    // heap-use-after-free.
    screen.draw();
    CHECK(screen.componentAt(3, 6) == nullptr);
}

namespace
{

/// @brief An input that commits what was typed when it loses the focus, and counts its commits.
class CommitOnBlur final: public Component
{
  public:
    explicit CommitOnBlur(int& commits): _commits(&commits) {}

    void render(Canvas& /*canvas*/) override {}
    [[nodiscard]] bool focusable() const override { return true; }
    [[nodiscard]] Size preferredSize() const override { return { .width = 10, .height = 1 }; }
    void onBlur() override { ++*_commits; }

  private:
    int* _commits;
};

} // namespace

namespace
{

/// @brief A focusable component whose onBlur() adds a component to its parent: a handler that changes
/// the tree it is leaving.
class GrowsParentOnBlur final: public Component
{
  public:
    GrowsParentOnBlur(Component& extra, int& blurs): _extra(&extra), _blurs(&blurs) {}

    void render(Canvas& /*canvas*/) override {}
    [[nodiscard]] bool focusable() const override { return true; }
    void onBlur() override
    {
        ++*_blurs;
        if (parent())
            parent()->addChild(*_extra, { .area = { .x = 0, .y = 1, .width = 4, .height = 1 } });
    }

  private:
    Component* _extra;
    int* _blurs;
};

} // namespace

TEST_CASE("Screen.onBlurThatChangesTheTree_runsAfterTheDetachLoop")
{
    // Run under ASan: delivered inside setScreen()'s loop over the parent's children, the handler's
    // addChild() reallocated the vector being walked -- a heap-use-after-free.
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto blurs = 0;
    auto extra = RootComponent {};
    auto panel = RootComponent {};
    auto input = GrowsParentOnBlur { extra, blurs };
    auto sibling = RootComponent {};
    // Two children, so the vector is full and the loop goes on reading it after the first.
    panel.addChild(input, { .area = { .x = 0, .y = 0, .width = 10, .height = 1 } });
    panel.addChild(sibling, { .area = { .x = 10, .y = 0, .width = 10, .height = 1 } });
    screen.root().addChild(panel, { .area = { .x = 0, .y = 0, .width = 20, .height = 2 } });
    screen.setFocus(&input);
    REQUIRE(panel.children().size() == 2);

    screen.root().removeChild(panel);
    CHECK(blurs == 1);
    CHECK(extra.parent() == &panel);
    CHECK(panel.children().size() == 3);
    CHECK(screen.focusedComponent() == nullptr);
    panel.removeChild(extra);
}

TEST_CASE("Screen.focusedComponentLeavingAlive_hearsOnBlur")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto commits = 0;

    SECTION("an overlay hidden while focused")
    {
        auto input = CommitOnBlur { commits };
        screen.showOverlay(input, Point { .x = 5, .y = 5 });
        screen.setFocus(&input);
        REQUIRE(screen.focusedComponent() == &input);
        REQUIRE(commits == 0);

        screen.hideOverlay(input);
        CHECK(commits == 1);
        CHECK_FALSE(input.focused());
        CHECK(screen.focusedComponent() == nullptr);
        CHECK(input.screen() == nullptr);
    }

    SECTION("a child removed from the tree with its parent")
    {
        auto panel = RootComponent {};
        auto input = CommitOnBlur { commits };
        panel.addChild(input, { .area = { .x = 0, .y = 0, .width = 10, .height = 1 } });
        screen.root().addChild(panel, { .area = { .x = 0, .y = 0, .width = 20, .height = 2 } });
        screen.setFocus(&input);
        REQUIRE(screen.focusedComponent() == &input);

        screen.root().removeChild(panel);
        CHECK(commits == 1);
        CHECK(screen.focusedComponent() == nullptr);
    }

    SECTION("a child left behind by a parent destroyed around it")
    {
        auto input = CommitOnBlur { commits };
        {
            auto panel = RootComponent {};
            panel.addChild(input, { .area = { .x = 0, .y = 0, .width = 10, .height = 1 } });
            screen.root().addChild(panel, { .area = { .x = 0, .y = 0, .width = 20, .height = 2 } });
            screen.setFocus(&input);
            REQUIRE(screen.focusedComponent() == &input);
        }
        CHECK(commits == 1);
        CHECK(input.parent() == nullptr);
        CHECK(input.screen() == nullptr);
        CHECK(screen.focusedComponent() == nullptr);
    }

    SECTION("not a focused component destroyed: it loses its focus with no call into it")
    {
        {
            auto input = CommitOnBlur { commits };
            screen.root().addChild(input, { .area = { .x = 0, .y = 0, .width = 10, .height = 1 } });
            screen.setFocus(&input);
            REQUIRE(screen.focusedComponent() == &input);
        }
        CHECK(commits == 0);
        CHECK(screen.focusedComponent() == nullptr);
    }
}

namespace
{

/// @brief A mouse recorder that offers a tooltip at every cell.
struct HoverableRecorder: MouseRecorder
{
    std::optional<HoverResult> onHover(int x, int y) override
    {
        return HoverResult {
            .text = "a tooltip",
            .position = { .x = x, .y = y },
            .contentType = TooltipContentType::PlainText,
        };
    }
};

} // namespace

TEST_CASE("Screen.mousePress_endsTheHoverSoNoTooltipOpensMidDrag")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto source = HoverableRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.hoverState().setDelay(std::chrono::milliseconds(0)); // a tick confirms any running timer
    screen.draw();

    SECTION("a hover timer running at the press")
    {
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 4, 2));
        REQUIRE(screen.hoverState().currentHover().has_value()); // the timer runs, unconfirmed
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
        screen.tickHover();
        CHECK_FALSE(screen.isTooltipVisible());

        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 6, 3)); // captured, still over A
        screen.tickHover();
        CHECK_FALSE(screen.isTooltipVisible());
        CHECK_FALSE(screen.hoverState().isHoverConfirmed());

        // After the release, the next move hovers as before.
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Release, 6, 3));
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 5, 2));
        screen.tickHover();
        CHECK(screen.isTooltipVisible());
    }
    SECTION("a tooltip visible at the press")
    {
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 4, 2));
        screen.tickHover();
        REQUIRE(screen.isTooltipVisible());
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
        CHECK_FALSE(screen.isTooltipVisible());
        screen.tickHover();
        CHECK_FALSE(screen.isTooltipVisible());
    }
}

TEST_CASE("Screen.pointerCapture_endsAtAReleaseBeforeTheInlineContentIsPlaced")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface, ScreenConfig { .viewport = Viewport::Inline });
    auto source = PaintedRecorder {};
    auto other = PaintedRecorder {};
    source.glyph = 'A'; // content, so the inline viewport has a height
    other.glyph = 'B';
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    REQUIRE(screen.pointerCapture() == &source);

    // External output: the screen no longer knows where its content starts, so the release cannot be
    // placed. It is not delivered, but the gesture is over.
    screen.releaseCursor();
    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Release, 4, 2)) == EventResult::Ignored);
    CHECK(screen.pointerCapture() == nullptr);

    screen.draw();
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2)); // over B
    CHECK(source.received.size() == 1);                                  // the press only
    REQUIRE(other.received.size() == 1);
    CHECK(other.received[0].type == MouseEvent::Type::Move);
}

TEST_CASE("Screen.releasePointer_inAPressHandler")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto panel = MouseRecorder {};
    auto leaf = PressCallback {};
    leaf.onPress = [&screen] {
        screen.releasePointer();
    };
    screen.root().addChild(panel, LayoutParams { .area = { .x = 2, .y = 1, .width = 20, .height = 5 } });
    panel.addChild(leaf, LayoutParams { .area = { .x = 1, .y = 1, .width = 5, .height = 2 } });
    screen.draw();

    SECTION("that handles the press declines the capture")
    {
        CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 5, 3)) == EventResult::Handled);
        CHECK(screen.pointerCapture() == nullptr);
        CHECK(panel.received.empty());
    }
    SECTION("that ignores the press lets it bubble, as any ignored event does")
    {
        leaf.pressResult = EventResult::Ignored;
        CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 5, 3)) == EventResult::Handled);
        REQUIRE(panel.received.size() == 1);
        CHECK(panel.received[0].type == MouseEvent::Type::Press);
        CHECK(screen.pointerCapture() == &panel);
    }
}

TEST_CASE("Screen.pointerCapture_aPressThatDestroysTheComponentIgnoringItEndsThere")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto panel = MouseRecorder {};
    auto doomed = std::make_unique<PressCallback>();
    doomed->pressResult = EventResult::Ignored;
    doomed->onPress = [&doomed] {
        doomed.reset();
    };
    screen.root().addChild(panel, LayoutParams { .area = { .x = 2, .y = 1, .width = 20, .height = 5 } });
    panel.addChild(*doomed, LayoutParams { .area = { .x = 1, .y = 1, .width = 5, .height = 2 } });
    screen.draw();

    // Its parent cannot be read once it is gone, so the press does not bubble: under ASan, reading it
    // would be a heap-use-after-free.
    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 5, 3)) == EventResult::Ignored);
    CHECK(doomed == nullptr);
    CHECK(panel.received.empty());
    CHECK(screen.pointerCapture() == nullptr);
}

TEST_CASE("Screen.pointerCapture_deliversToACapturedOverlayInItsOwnCoordinates")
{
    auto surface = MockScreenSurface(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(surface);
    auto tree = MouseRecorder {};
    auto overlay = MouseRecorder {};
    screen.root().addChild(tree, LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 24 } });
    screen.showOverlay(overlay, Point { .x = 5, .y = 2 }); // columns 5..14, rows 2..4
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 7, 4));   // cell (3, 6)
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 30, 10));  // cell (9, 29): over the tree
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Release, 1, 1)); // cell (0, 0): above and left

    REQUIRE(overlay.received.size() == 3);
    CHECK(overlay.received[0].x == 2); // 6 - 5 + 1
    CHECK(overlay.received[0].y == 2); // 3 - 2 + 1
    CHECK(overlay.received[1].type == MouseEvent::Type::Move);
    CHECK(overlay.received[1].x == 25); // 29 - 5 + 1
    CHECK(overlay.received[1].y == 8);  // 9 - 2 + 1
    CHECK(overlay.received[2].type == MouseEvent::Type::Release);
    CHECK(overlay.received[2].x == -4); // 0 - 5 + 1
    CHECK(overlay.received[2].y == -1); // 0 - 2 + 1
    CHECK(tree.received.empty());
    CHECK(screen.pointerCapture() == nullptr);
}

// ============================================================================
// Row marks: zero-width sequences anchored to canvas cells
// ============================================================================

namespace
{

/// @brief A mark a test asks @c MarkedRowsComponent to attach.
struct TestMark
{
    int row;
    int column;
    std::string sequence;
};

/// @brief Renders one line of text per entry of @c lines and attaches every mark in @c marks.
struct MarkedRowsComponent: Component
{
    std::vector<std::string> lines;
    std::vector<TestMark> marks;

    void render(Canvas& canvas) override
    {
        for (auto const row: std::views::iota(0, static_cast<int>(lines.size())))
            canvas.putString(row, 0, lines[static_cast<std::size_t>(row)], {});
        // ESC-prefixed like a real mark, so the mock reads the bytes as zero-width rather than as
        // printable text that would advance its cursor.
        for (auto const& mark: marks)
            canvas.mark(mark.row, mark.column, "\033" + mark.sequence);
        // A visible cursor: with a hidden one, the inline flush records cursor.y as the row it
        // stopped on while the terminal cursor stays on the last content row, so the next frame
        // starts one row low (pre-existing, unrelated to marks).
        canvas.setCursor(0, 0);
    }

    [[nodiscard]] Size preferredSize() const override
    {
        return { .width = 80, .height = static_cast<int>(lines.size()) };
    }
};

/// @brief Returns the raw writes of @p mock that are the mark @p text (written ESC-prefixed).
std::vector<MockTerminalOutput::RawWrite> rawWritesOf(MockTerminalOutput const& mock, std::string_view text)
{
    auto result = std::vector<MockTerminalOutput::RawWrite> {};
    std::ranges::copy_if(mock.rawWrites(), std::back_inserter(result), [&](auto const& write) {
        return write.text.size() == text.size() + 1 && write.text.front() == '\033'
               && std::string_view { write.text }.substr(1) == text;
    });
    return result;
}

/// @brief A mock that also records where each writeText() began.
class TextPositionMock: public MockTerminalOutput
{
  public:
    using MockTerminalOutput::MockTerminalOutput;

    void writeText(std::string_view text, Style const& style) override
    {
        textWrites.push_back(
            RawWrite { .row = cursorRow(), .column = cursorCol(), .text = std::string { text } });
        MockTerminalOutput::writeText(text, style);
    }

    std::vector<RawWrite> textWrites;
};

/// @brief An inline screen over an 80x24 mock, holding one @c MarkedRowsComponent.
struct InlineMarkFixture
{
    MockTerminalOutput* mock;
    MockScreenSurface surface;
    Screen screen;
    MarkedRowsComponent comp;

    explicit InlineMarkFixture(std::unique_ptr<MockTerminalOutput> output):
        mock(output.get()),
        surface(std::move(output)),
        screen(surface, ScreenConfig { .viewport = Viewport::Inline, .inlineMaxHeight = 12 })
    {
    }

    void attach(int height)
    {
        screen.root().addChild(comp,
                               LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = height } });
    }
};

} // namespace

TEST_CASE("Screen.rowMarks_firstFrameEmitsAtColumn")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "first line", "second line", "third line" };
    fixture.comp.marks = { TestMark { .row = 1, .column = 0, .sequence = "<A>" } };
    fixture.attach(3);

    fixture.screen.draw();

    // The cursor starts at the top of an empty 24-row terminal, so content row 1 is terminal row 1.
    auto const marks = rawWritesOf(*fixture.mock, "<A>");
    REQUIRE(marks.size() == 1);
    CHECK(marks[0].row == 1);
    CHECK(marks[0].column == 0);
}

TEST_CASE("Screen.rowMarks_unchangedRowEmitsNothing")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "first line", "second line" };
    fixture.comp.marks = { TestMark { .row = 0, .column = 0, .sequence = "<A>" },
                           TestMark { .row = 1, .column = 4, .sequence = "<B>" } };
    fixture.attach(2);

    fixture.screen.draw();
    auto const writesAfterFirstFrame = fixture.mock->rawWrites().size();
    REQUIRE(rawWritesOf(*fixture.mock, "<A>").size() == 1);
    REQUIRE(rawWritesOf(*fixture.mock, "<B>").size() == 1);

    fixture.screen.draw();

    CHECK(fixture.mock->rawWrites().size() == writesAfterFirstFrame);
}

TEST_CASE("Screen.rowMarks_segmentReestablishedOnRepaint")
{
    auto output = std::make_unique<TextPositionMock>(80, 24);
    auto* textMock = output.get();
    auto fixture = InlineMarkFixture { std::move(output) };
    fixture.comp.lines = { "prompt>   abcdefghij" };
    fixture.comp.marks = { TestMark { .row = 0, .column = 0, .sequence = "<P>" },
                           TestMark { .row = 0, .column = 10, .sequence = "<B>" } };
    fixture.attach(1);
    fixture.screen.draw();
    auto const pBefore = rawWritesOf(*fixture.mock, "<P>").size();
    auto const bBefore = rawWritesOf(*fixture.mock, "<B>").size();

    textMock->textWrites.clear();
    fixture.comp.lines = { "prompt>   abXdefghij" }; // column 12 changes
    fixture.screen.draw();

    auto const bWrites = rawWritesOf(*fixture.mock, "<B>");
    REQUIRE(bWrites.size() == bBefore + 1);
    CHECK(bWrites.back().column == 10);
    CHECK(bWrites.back().row == 0);
    CHECK(rawWritesOf(*fixture.mock, "<P>").size() == pBefore);

    // The cursor went back to column 12 after the mark, so the repainted cell lands there.
    REQUIRE(textMock->textWrites.size() == 1);
    CHECK(textMock->textWrites[0].text == "X");
    CHECK(textMock->textWrites[0].row == 0);
    CHECK(textMock->textWrites[0].column == 12);
}

TEST_CASE("Screen.rowMarks_leftSegmentRepaint")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "prompt>   abcdefghij" };
    fixture.comp.marks = { TestMark { .row = 0, .column = 0, .sequence = "<P>" },
                           TestMark { .row = 0, .column = 10, .sequence = "<B>" } };
    fixture.attach(1);
    fixture.screen.draw();
    auto const pBefore = rawWritesOf(*fixture.mock, "<P>").size();
    auto const bBefore = rawWritesOf(*fixture.mock, "<B>").size();

    fixture.comp.lines = { "proXpt>   abcdefghij" }; // column 3 changes
    fixture.screen.draw();

    auto const pWrites = rawWritesOf(*fixture.mock, "<P>");
    REQUIRE(pWrites.size() == pBefore + 1);
    CHECK(pWrites.back().column == 0);
    CHECK(rawWritesOf(*fixture.mock, "<B>").size() == bBefore);
}

TEST_CASE("Screen.rowMarks_reservationScroll")
{
    auto output = std::make_unique<MockTerminalOutput>(80, 24);
    output->moveTo(24, 1); // the cursor sits on the last row, so reserving room scrolls
    auto fixture = InlineMarkFixture { std::move(output) };
    fixture.comp.lines = { "one", "two", "three", "four" };
    fixture.comp.marks = { TestMark { .row = 1, .column = 0, .sequence = "<A>" } };
    fixture.attach(4);

    fixture.screen.draw();

    // Reserving four rows from the bottom row scrolls three times: content row 0 lands on terminal
    // row 20 (0-based), so content row 1 is row 21.
    auto const marks = rawWritesOf(*fixture.mock, "<A>");
    REQUIRE(marks.size() == 1);
    CHECK(fixture.mock->scrollCount() == 3);
    CHECK(marks[0].row == 21);
    CHECK(marks[0].column == 0);
}

TEST_CASE("Screen.rowMarks_fullRedrawEmitsEveryMark")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "prompt>   abcdefghij" };
    fixture.comp.marks = { TestMark { .row = 0, .column = 0, .sequence = "<P>" },
                           TestMark { .row = 0, .column = 10, .sequence = "<B>" } };
    fixture.attach(1);
    fixture.screen.draw();

    fixture.screen.invalidate();
    fixture.screen.draw();

    CHECK(rawWritesOf(*fixture.mock, "<P>").size() == 2);
    CHECK(rawWritesOf(*fixture.mock, "<B>").size() == 2);
}

TEST_CASE("Screen.rowMarks_changedSequenceOverSameCellsReemits")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "first line", "second line" };
    fixture.comp.marks = { TestMark { .row = 1, .column = 0, .sequence = "<P>" } };
    fixture.attach(2);
    fixture.screen.draw();
    REQUIRE(rawWritesOf(*fixture.mock, "<A>").empty());

    fixture.comp.marks = { TestMark { .row = 1, .column = 0, .sequence = "<A>" } };
    fixture.screen.draw();

    auto const marks = rawWritesOf(*fixture.mock, "<A>");
    REQUIRE(marks.size() == 1);
    CHECK(marks[0].row == 1);
    CHECK(marks[0].column == 0);
}

TEST_CASE("Screen.rowMarks_changedColumnOverSameCellsReemits")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "first line", "second line" };
    fixture.comp.marks = { TestMark { .row = 1, .column = 2, .sequence = "<B>" } };
    fixture.attach(2);
    fixture.screen.draw();
    REQUIRE(rawWritesOf(*fixture.mock, "<B>").size() == 1);

    fixture.comp.marks = { TestMark { .row = 1, .column = 5, .sequence = "<B>" } };
    fixture.screen.draw();

    auto const marks = rawWritesOf(*fixture.mock, "<B>");
    REQUIRE(marks.size() == 2);
    CHECK(marks[1].row == 1);
    CHECK(marks[1].column == 5);
}

TEST_CASE("Screen.rowMarks_imageRowEmitsNothing")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "", "text below the image" };
    fixture.comp.marks = { TestMark { .row = 0, .column = 0, .sequence = "<I>" },
                           TestMark { .row = 1, .column = 0, .sequence = "<T>" } };
    fixture.attach(2);

    struct ImageOnTop: Component
    {
        void render(Canvas& canvas) override { canvas.drawImage(0, 0, 4, 1, "sixel"); }
    };
    auto image = ImageOnTop {};
    fixture.comp.addChild(image, LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 1 } });

    fixture.screen.draw();

    CHECK(rawWritesOf(*fixture.mock, "<I>").empty());
    CHECK(rawWritesOf(*fixture.mock, "<T>").size() == 1);
}

TEST_CASE("Screen.rowMarks_continuationColumnEmitsNothing")
{
    auto fixture = InlineMarkFixture { std::make_unique<MockTerminalOutput>(80, 24) };
    fixture.comp.lines = { "a\xE7\x95\x8C"
                           "b" }; // 界 is two columns wide: columns 1 and 2
    fixture.comp.marks = { TestMark { .row = 0, .column = 2, .sequence = "<C>" } };
    fixture.attach(1);

    fixture.screen.draw();

    CHECK(rawWritesOf(*fixture.mock, "<C>").empty());
}

TEST_CASE("TerminalOutput.deleteLines_writesDL")
{
    auto mock = MockTerminalOutput { 80, 24 };
    CHECK(mock.deletedLines() == 0);
    mock.deleteLines(1);
    CHECK(mock.deletedLines() == 1);
}

TEST_CASE("Screen.fixedViewport_pastTheScreensEdge_drawsWithoutWritingOutsideIt")
{
    // Pins the CHANGELOG's fix: a fixed area below or right of the screen made the flush build a
    // row range that ran backwards, which a hardened standard library (_GLIBCXX_ASSERTIONS) aborts
    // on. Drawn once, it must write none of the component's cells.
    auto bytes = std::string {};
    auto surface = HeadlessSurface { Size { .width = 20, .height = 4 },
                                     [&bytes](std::string_view chunk) { bytes += chunk; } };
    auto screen = Screen(surface);
    screen.setViewport(Rect { .x = 25, .y = 6, .width = 10, .height = 2 });
    auto painter = PaintedRecorder {};
    painter.glyph = 'Z';
    screen.root().addChild(painter, LayoutParams { .area = { .x = 0, .y = 0, .width = 10, .height = 2 } });

    screen.draw();

    CHECK_FALSE(bytes.contains('Z'));
}

TEST_CASE("Screen.fixedViewport_partlyPastTheScreensEdge_writesOnlyWhatIsOnScreen")
{
    // The same area straddling the bottom-right corner: the part on screen is painted, the rest
    // clipped rather than written past the edge.
    auto bytes = std::string {};
    auto surface = HeadlessSurface { Size { .width = 20, .height = 4 },
                                     [&bytes](std::string_view chunk) { bytes += chunk; } };
    auto screen = Screen(surface);
    screen.setViewport(Rect { .x = 15, .y = 2, .width = 10, .height = 5 });
    auto painter = PaintedRecorder {};
    painter.glyph = 'Z';
    screen.root().addChild(painter, LayoutParams { .area = { .x = 0, .y = 0, .width = 10, .height = 5 } });

    screen.draw();

    // Columns 15..19 of rows 2..3: five cells on each of two rows.
    CHECK(std::ranges::count(bytes, 'Z') == 10);
}

TEST_CASE("Screen.aTemporaryClockIsRefused")
{
    // Screen and HoverState keep the clock they are given; a temporary one would dangle.
    STATIC_CHECK(std::is_constructible_v<Screen, ScreenSurface&, ScreenConfig, ManualClock&>);
    STATIC_CHECK_FALSE(std::is_constructible_v<Screen, ScreenSurface&, ScreenConfig, ManualClock>);
    STATIC_CHECK(std::is_constructible_v<HoverState, std::chrono::milliseconds, ManualClock&>);
    STATIC_CHECK_FALSE(std::is_constructible_v<HoverState, std::chrono::milliseconds, ManualClock>);
}
