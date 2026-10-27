// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/CaptionHitTest.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <vector>

using namespace contour::platform;
using contour::config::WindowControlKind;

namespace
{

constexpr auto MinimizeRect = LogicalRect { .x = 868, .y = 0, .width = 46, .height = 40 };
constexpr auto MaximizeRect = LogicalRect { .x = 914, .y = 0, .width = 46, .height = 40 };
constexpr auto CloseRect = LogicalRect { .x = 960, .y = 0, .width = 46, .height = 40 };

/// @return The Fluent row's trailing caption buttons, minimise, maximise and close, in a 1006-pixel window.
[[nodiscard]] CaptionLayout fluentLayout()
{
    auto layout = CaptionLayout { .buttons = {}, .windowWidth = 1006, .resizeBorder = 4 };
    layout.buttons[static_cast<size_t>(WindowControlKind::Minimize)] = MinimizeRect;
    layout.buttons[static_cast<size_t>(WindowControlKind::Maximize)] = MaximizeRect;
    layout.buttons[static_cast<size_t>(WindowControlKind::Close)] = CloseRect;
    return layout;
}

constexpr auto Placements =
    std::array { WindowPlacement::Windowed, WindowPlacement::Maximized, WindowPlacement::FullScreen };
constexpr auto Visuals =
    std::array { CaptionButtonVisual::Rest, CaptionButtonVisual::Hover, CaptionButtonVisual::Pressed };
constexpr auto Hits = std::array { CaptionHit::PassThrough, CaptionHit::MaximizeButton };

using enum CaptionHit;
using enum NcEvent;

} // namespace

TEST_CASE("captionHit.maximizeOnly", "[captionhit]")
{
    auto const layout = fluentLayout();
    for (auto const placement: Placements)
    {
        CAPTURE(static_cast<int>(placement));
        for (auto const y: std::views::iota(4, 40))
            for (auto const x: std::views::iota(914, 960))
                REQUIRE(captionHitAt({ .x = double(x), .y = double(y) }, layout, placement)
                        == MaximizeButton);
        CHECK(captionHitAt({ .x = 890, .y = 20 }, layout, placement) == PassThrough); // minimise
        CHECK(captionHitAt({ .x = 980, .y = 20 }, layout, placement) == PassThrough); // close
        CHECK(captionHitAt({ .x = 913.9, .y = 20 }, layout, placement) == PassThrough);
        CHECK(captionHitAt({ .x = 960, .y = 20 }, layout, placement) == PassThrough); // half-open
        CHECK(captionHitAt({ .x = 930, .y = 40 }, layout, placement) == PassThrough);
        CHECK(captionHitAt({ .x = 400, .y = 20 }, layout, placement) == PassThrough);  // the drag region
        CHECK(captionHitAt({ .x = 400, .y = 300 }, layout, placement) == PassThrough); // the content
    }

    auto noMaximize = layout;
    noMaximize.buttons[static_cast<size_t>(WindowControlKind::Maximize)] = std::nullopt;
    for (auto const placement: Placements)
        for (auto const point:
             { LogicalPoint { 930, 20 }, LogicalPoint { 930, 0 }, LogicalPoint { 400, 300 } })
            CHECK(captionHitAt(point, noMaximize, placement) == PassThrough);
}

TEST_CASE("captionHit.windowedKeepsTheResizeStrip", "[captionhit]")
{
    auto const layout = fluentLayout();
    CHECK(captionHitAt({ .x = 930, .y = 2 }, layout, WindowPlacement::Windowed) == PassThrough);
    CHECK(captionHitAt({ .x = 930, .y = 3.9 }, layout, WindowPlacement::Windowed) == PassThrough);
    CHECK(captionHitAt({ .x = 930, .y = 4 }, layout, WindowPlacement::Windowed) == MaximizeButton);
    CHECK(captionHitAt({ .x = 930, .y = 2 }, layout, WindowPlacement::Maximized) == MaximizeButton);
    CHECK(captionHitAt({ .x = 930, .y = 2 }, layout, WindowPlacement::FullScreen) == MaximizeButton);
}

TEST_CASE("captionHit.maximizedReachesTheEdge", "[captionhit]")
{
    auto layout = fluentLayout();
    CHECK(captionHitAt({ .x = 930, .y = 0 }, layout, WindowPlacement::Maximized) == MaximizeButton);
    CHECK(captionHitAt({ .x = 930, .y = -1 }, layout, WindowPlacement::Maximized) == MaximizeButton);
    CHECK(captionHitAt({ .x = 930, .y = -1 }, layout, WindowPlacement::Windowed) == PassThrough);
    // Not at the trailing edge, so nothing beyond the rectangle's own right side.
    CHECK(captionHitAt({ .x = 960, .y = 10 }, layout, WindowPlacement::Maximized) == PassThrough);

    layout.buttons[static_cast<size_t>(WindowControlKind::Maximize)] = CloseRect;
    layout.buttons[static_cast<size_t>(WindowControlKind::Close)] = std::nullopt;
    CHECK(captionHitAt({ .x = 1005, .y = 10 }, layout, WindowPlacement::Maximized) == MaximizeButton);
    CHECK(captionHitAt({ .x = 1006, .y = 10 }, layout, WindowPlacement::Maximized) == MaximizeButton);
    CHECK(captionHitAt({ .x = 1006, .y = 10 }, layout, WindowPlacement::FullScreen) == MaximizeButton);
    CHECK(captionHitAt({ .x = 1006, .y = 10 }, layout, WindowPlacement::Windowed) == PassThrough);

    // A button not at the top keeps its top edge, maximised too.
    auto lowered = fluentLayout();
    lowered.buttons[static_cast<size_t>(WindowControlKind::Maximize)] =
        LogicalRect { .x = 914, .y = 6, .width = 46, .height = 34 };
    CHECK(captionHitAt({ .x = 930, .y = 5 }, lowered, WindowPlacement::Maximized) == PassThrough);
}

TEST_CASE("captionHit.sameZoneAtEveryScale", "[captionhit]")
{
    auto const layout = fluentLayout();
    CHECK(toLogical({ .x = 3, .y = 3 }, 1.5) == LogicalPoint { 2, 2 });
    constexpr auto Probes = std::array { LogicalPoint { 937, 20 },
                                         LogicalPoint { 890, 20 },
                                         LogicalPoint { 980, 20 },
                                         LogicalPoint { 400, 300 } };
    for (auto const dpr: { 1.0, 1.25, 1.5, 2.0 })
        for (auto const placement: Placements)
            for (auto const& probe: Probes)
            {
                CAPTURE(dpr, probe.x, probe.y, static_cast<int>(placement));
                auto const physical = PhysicalPoint { .x = static_cast<int>(probe.x * dpr),
                                                      .y = static_cast<int>(probe.y * dpr) };
                CHECK(captionHitAt(toLogical(physical, dpr), layout, placement)
                      == captionHitAt(probe, layout, placement));
            }
    CHECK(captionHitAt({ .x = 937, .y = 20 }, layout, WindowPlacement::Windowed) == MaximizeButton);
}

TEST_CASE("captionTransitions.hoverThenLeave", "[captionhit]")
{
    using enum CaptionButtonVisual;
    CHECK(onNcEvent(MouseMove, MaximizeButton, Rest)
          == NcTransition { Hover, NcEffect::TrackLeave, NcDisposition::DefaultHandling });
    CHECK(onNcEvent(MouseMove, MaximizeButton, Hover)
          == NcTransition { Hover, NcEffect::None, NcDisposition::DefaultHandling });
    CHECK(onNcEvent(MouseLeave, PassThrough, Hover)
          == NcTransition { Rest, NcEffect::None, NcDisposition::DefaultHandling });
}

TEST_CASE("captionTransitions.clickTogglesOnce", "[captionhit]")
{
    using enum CaptionButtonVisual;
    auto const down = onNcEvent(LeftDown, MaximizeButton, Hover);
    CHECK(down == NcTransition { Pressed, NcEffect::None, NcDisposition::Swallow });
    auto const up = onNcEvent(LeftUp, MaximizeButton, down.visual);
    CHECK(up == NcTransition { Hover, NcEffect::ToggleMaximize, NcDisposition::Swallow });
    auto const again = onNcEvent(LeftUp, MaximizeButton, up.visual);
    CHECK(again.effect == NcEffect::None);
    CHECK(again.disposition == NcDisposition::Swallow);
}

TEST_CASE("captionTransitions.dragOffCancels", "[captionhit]")
{
    using enum CaptionButtonVisual;
    SECTION("moved off the button, released elsewhere")
    {
        auto const down = onNcEvent(LeftDown, MaximizeButton, Hover);
        auto const off = onNcEvent(MouseMove, PassThrough, down.visual);
        CHECK(off.visual == Rest);
        CHECK(onNcEvent(LeftUp, PassThrough, off.visual)
              == NcTransition { Rest, NcEffect::None, NcDisposition::DefaultHandling });
    }
    SECTION("left the non-client area, came back and released on the button")
    {
        auto const down = onNcEvent(LeftDown, MaximizeButton, Hover);
        auto const left = onNcEvent(MouseLeave, PassThrough, down.visual);
        CHECK(left.visual == Rest);
        CHECK(onNcEvent(LeftUp, MaximizeButton, left.visual).effect == NcEffect::None);
    }
}

TEST_CASE("captionTransitions.doubleClickIsAPress", "[captionhit]")
{
    using enum CaptionButtonVisual;
    CHECK(onNcEvent(LeftDoubleClick, MaximizeButton, Hover)
          == NcTransition { Pressed, NcEffect::None, NcDisposition::Swallow });
}

TEST_CASE("captionTransitions.unmatchedKeepsTheVisual", "[captionhit]")
{
    for (auto const visual: Visuals)
        CHECK(onNcEvent(LeftDown, PassThrough, visual)
              == NcTransition { visual, NcEffect::None, NcDisposition::DefaultHandling });
}

TEST_CASE("captionTransitions.everyRowIsReachable", "[captionhit]")
{
    for (auto const index: std::views::iota(size_t { 0 }, CaptionTransitions.size()))
    {
        CAPTURE(index);
        auto const& row = CaptionTransitions[index];
        auto const hits = row.hit ? std::vector { *row.hit } : std::vector(Hits.begin(), Hits.end());
        auto const visualsBefore =
            row.before ? std::vector { *row.before } : std::vector(Visuals.begin(), Visuals.end());
        auto reached = false;
        for (auto const hit: hits)
            for (auto const before: visualsBefore)
            {
                // Unless an earlier row claims the key, the row answers it.
                if (findNcTransition(row.event, hit, before) != &row)
                    continue;
                reached = true;
                CHECK(onNcEvent(row.event, hit, before) == row.after);
            }
        CHECK(reached);
    }
}
