// SPDX-License-Identifier: Apache-2.0
#include <contour/window/QuickTerminalPolicy.hpp>
#include <contour/window/WindowLiveness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <ranges>

using namespace contour::window;
using contour::config::MotionStyle;
using contour::config::MotionTokens;
using contour::config::QuickTerminalFocusLoss;
using contour::config::ReducedMotionCrossFadeMs;

namespace
{
constexpr auto AllEvents = std::array { QuickTerminalEvent::Hotkey,
                                        QuickTerminalEvent::FocusLost,
                                        QuickTerminalEvent::FocusGained,
                                        QuickTerminalEvent::AnimationDone,
                                        QuickTerminalEvent::LastTabClosed };
constexpr auto AllFocus = std::array { QuickTerminalFocus::Unfocused, QuickTerminalFocus::Focused };
constexpr auto AllLoss = std::array { QuickTerminalFocusLoss::Hide, QuickTerminalFocusLoss::Stay };

struct Outcome
{
    QuickTerminalState to;
    QuickTerminalEffect effect;
    [[nodiscard]] bool operator==(Outcome const&) const = default;
};

[[nodiscard]] std::optional<Outcome> outcomeOf(QuickTerminalState from,
                                               QuickTerminalEvent event,
                                               QuickTerminalFocus focus = QuickTerminalFocus::Focused,
                                               QuickTerminalFocusLoss loss = QuickTerminalFocusLoss::Hide)
{
    auto const step = quickTerminalStep(from, event, focus, loss);
    if (!step)
        return std::nullopt;
    return Outcome { step->to, step->effect };
}
} // namespace

TEST_CASE("quickTerminal.framesTakeFortyPercentOfTheAvailableHeight", "[quickterminal]")
{
    auto const frames = quickTerminalFrames({ 0, 25, 1512, 957 });
    CHECK(frames.shown == ScreenRect { 0, 25, 1512, 383 });
    CHECK(frames.hidden == ScreenRect { 0, -358, 1512, 383 });

    auto const left = quickTerminalFrames({ -1920, 0, 1920, 1080 });
    CHECK(left.shown == ScreenRect { -1920, 0, 1920, 432 });
    CHECK(left.hidden == ScreenRect { -1920, -432, 1920, 432 });

    CHECK(quickTerminalFrames({ 0, 0, 800, 1001 }).shown.height == 400);
}

TEST_CASE("quickTerminal.everyStateAndEventHasAtMostOneRow", "[quickterminal]")
{
    for (auto const& row: QuickTerminalTransitions)
    {
        auto const matches = std::ranges::count_if(QuickTerminalTransitions, [&](auto const& other) {
            return other.from == row.from && other.event == row.event && other.focus == row.focus
                   && other.focusLoss == row.focusLoss;
        });
        CHECK(matches == 1);
    }
}

TEST_CASE("quickTerminal.theTableCoversEveryCombinationItNames", "[quickterminal]")
{
    // Two rows to each of the 4 (focus, focus loss) combinations a wildcard row spreads over.
    auto const lastTabRows = std::ranges::count_if(QuickTerminalTransitions, [](auto const& row) {
        return row.event == QuickTerminalEvent::LastTabClosed;
    });
    CHECK(lastTabRows == 16); // every state but Absent
}

TEST_CASE("quickTerminal.transitions", "[quickterminal]")
{
    using S = QuickTerminalState;
    using E = QuickTerminalEvent;
    using X = QuickTerminalEffect;
    using F = QuickTerminalFocus;
    using L = QuickTerminalFocusLoss;

    for (auto const focus: AllFocus)
        for (auto const loss: AllLoss)
        {
            CHECK(outcomeOf(S::Absent, E::Hotkey, focus, loss) == Outcome { S::Showing, X::CreateAndShow });
            CHECK(outcomeOf(S::Hidden, E::Hotkey, focus, loss) == Outcome { S::Showing, X::Show });
            CHECK(outcomeOf(S::Showing, E::AnimationDone, focus, loss) == Outcome { S::Shown, X::None });
            CHECK(outcomeOf(S::Showing, E::Hotkey, focus, loss) == Outcome { S::Hiding, X::Reverse });
            CHECK(outcomeOf(S::Hiding, E::Hotkey, focus, loss) == Outcome { S::Showing, X::Reverse });
            CHECK(outcomeOf(S::Hiding, E::AnimationDone, focus, loss) == Outcome { S::Hidden, X::None });
        }

    CHECK(outcomeOf(S::Shown, E::Hotkey, F::Focused) == Outcome { S::Hiding, X::Hide });
    CHECK(outcomeOf(S::Shown, E::Hotkey, F::Unfocused) == Outcome { S::Shown, X::Focus });
    CHECK(outcomeOf(S::Shown, E::FocusLost, F::Unfocused, L::Hide) == Outcome { S::Hiding, X::Hide });
    CHECK(outcomeOf(S::Shown, E::FocusLost, F::Unfocused, L::Stay) == Outcome { S::Shown, X::None });
    CHECK_FALSE(outcomeOf(S::Hidden, E::FocusLost).has_value());
}

TEST_CASE("quickTerminal.focusLostDuringTheSlideInFollowsTheRule", "[quickterminal]")
{
    using S = QuickTerminalState;
    using E = QuickTerminalEvent;
    using X = QuickTerminalEffect;
    using L = QuickTerminalFocusLoss;

    // Hide mode turns the slide round, as a press would; Stay mode lets it finish.
    for (auto const focus: AllFocus)
    {
        CHECK(outcomeOf(S::Showing, E::FocusLost, focus, L::Hide) == Outcome { S::Hiding, X::Reverse });
        CHECK_FALSE(outcomeOf(S::Showing, E::FocusLost, focus, L::Stay).has_value());
    }
}

TEST_CASE("quickTerminal.everyLiveStateAnswersFocusLostByTheRule", "[quickterminal]")
{
    // The rule is "hide on focus loss": wherever the window is on the screen or coming onto it, Hide
    // mode leads to Hiding, and no state is left where losing the focus keeps a Hide-mode window up.
    for (auto const state: { QuickTerminalState::Showing, QuickTerminalState::Shown })
        for (auto const focus: AllFocus)
        {
            auto const step =
                quickTerminalStep(state, QuickTerminalEvent::FocusLost, focus, QuickTerminalFocusLoss::Hide);
            REQUIRE(step.has_value());
            CHECK(step->to == QuickTerminalState::Hiding);
        }
}

TEST_CASE("quickTerminal.theLastTabClosingEndsTheWindowFromAnyLiveState", "[quickterminal]")
{
    for (auto const state: { QuickTerminalState::Hidden,
                             QuickTerminalState::Showing,
                             QuickTerminalState::Shown,
                             QuickTerminalState::Hiding })
        CHECK(outcomeOf(state, QuickTerminalEvent::LastTabClosed)
              == Outcome { QuickTerminalState::Absent, QuickTerminalEffect::Destroy });
    CHECK_FALSE(outcomeOf(QuickTerminalState::Absent, QuickTerminalEvent::LastTabClosed).has_value());
}

TEST_CASE("quickTerminal.everyRowLeadsSomewhereAnEventIsAcceptedFrom", "[quickterminal]")
{
    // A pressed hotkey must never find the machine stuck: from every state it has a row.
    for (auto const state: detail::AllStates)
        for (auto const focus: AllFocus)
            for (auto const loss: AllLoss)
                CHECK(quickTerminalStep(state, QuickTerminalEvent::Hotkey, focus, loss).has_value());
    CHECK(std::ranges::size(AllEvents) == 5);
}

TEST_CASE("quickTerminal.motion", "[quickterminal]")
{
    auto const full = MotionTokens { .emphasizedMs = 220, .style = MotionStyle::Full };
    auto const reduced =
        MotionTokens { .emphasizedMs = ReducedMotionCrossFadeMs, .style = MotionStyle::CrossFade };

    auto const slide = quickTerminalAnimation(full, PlacementControl::Client, SlideClearance::Clear);
    CHECK(slide.motion == QuickTerminalMotion::Slide);
    CHECK(slide.duration == std::chrono::milliseconds(220));

    auto const fade = quickTerminalAnimation(reduced, PlacementControl::Client, SlideClearance::Clear);
    CHECK(fade.motion == QuickTerminalMotion::Fade);
    CHECK(fade.duration == std::chrono::milliseconds(100));

    auto const compositor = quickTerminalAnimation(full, PlacementControl::Compositor, SlideClearance::Clear);
    CHECK(compositor.motion == QuickTerminalMotion::Fade);
    CHECK(compositor.duration == std::chrono::milliseconds(220));
}

TEST_CASE("quickTerminal.aScreenStackedAboveBlocksTheSlide", "[quickterminal]")
{
    constexpr auto Lower = ScreenRect { .x = 0, .y = 0, .width = 1920, .height = 1080 };
    constexpr auto Upper = ScreenRect { .x = 0, .y = -1080, .width = 1920, .height = 1080 };
    constexpr auto Right = ScreenRect { .x = 1920, .y = 0, .width = 1920, .height = 1080 };
    constexpr auto Below = ScreenRect { .x = 0, .y = 1080, .width = 1920, .height = 1080 };
    constexpr auto Diagonal = ScreenRect { .x = 1920, .y = -1080, .width = 1920, .height = 1080 };
    auto const frames = quickTerminalFrames(Lower);

    // Alone, beside, below or diagonal to the target, nothing is in the hidden frame's way.
    CHECK(quickTerminalSlideClearance(frames, {}) == SlideClearance::Clear);
    for (auto const& neighbours: { std::array { Right }, std::array { Below }, std::array { Diagonal } })
        CHECK(quickTerminalSlideClearance(frames, neighbours) == SlideClearance::Clear);

    // A screen above shows the parked window, whether or not other screens are about.
    CHECK(quickTerminalSlideClearance(frames, std::array { Upper }) == SlideClearance::Blocked);
    CHECK(quickTerminalSlideClearance(frames, std::array { Right, Upper }) == SlideClearance::Blocked);

    // Seen from the upper screen the lower one is below it, so it may slide.
    CHECK(quickTerminalSlideClearance(quickTerminalFrames(Upper), std::array { Lower })
          == SlideClearance::Clear);

    // The menu bar's strip keeps the hidden frame overlapping the area above: the available geometry starts
    // below it.
    auto const withMenuBar = quickTerminalFrames({ 0, 25, 1920, 1055 });
    CHECK(quickTerminalSlideClearance(withMenuBar, std::array { Upper }) == SlideClearance::Blocked);

    // A blocked slide fades instead, however long the motion is.
    auto const full = MotionTokens { .emphasizedMs = 220, .style = MotionStyle::Full };
    auto const animation = quickTerminalAnimation(full, PlacementControl::Client, SlideClearance::Blocked);
    CHECK(animation.motion == QuickTerminalMotion::Fade);
    CHECK(animation.duration == std::chrono::milliseconds(220));
}

TEST_CASE("quickTerminal.touchingRectanglesDoNotOverlap", "[quickterminal]")
{
    CHECK_FALSE(overlaps({ 0, 0, 10, 10 }, { 10, 0, 10, 10 }));
    CHECK_FALSE(overlaps({ 0, 0, 10, 10 }, { 0, 10, 10, 10 }));
    CHECK(overlaps({ 0, 0, 10, 10 }, { 9, 9, 10, 10 }));
    CHECK(overlaps({ 0, 0, 10, 10 }, { 2, 2, 3, 3 }));
}

TEST_CASE("quickTerminal.onlyAQuickTerminalOnScreenKeepsTheAppRunning", "[quickterminal]")
{
    using S = QuickTerminalState;
    struct Row
    {
        S state;
        bool keeps;
    };
    // Ruling D1: a hidden quick terminal does not keep the app running; one coming, going or on screen does.
    constexpr auto Rows = std::array { Row { S::Absent, false },
                                       Row { S::Hidden, false },
                                       Row { S::Showing, true },
                                       Row { S::Shown, true },
                                       Row { S::Hiding, true } };
    static_assert(Rows.size() == detail::AllStates.size());
    for (auto const& row: Rows)
    {
        CHECK(quickTerminalKeepsAppAlive(row.state) == row.keeps);

        // As the last-window rule sees it with no other window: the panel is counted by its state, not by
        // whether the platform still calls its parked window visible.
        auto const visibility =
            quickTerminalKeepsAppAlive(row.state) ? WindowVisibility::Shown : WindowVisibility::Hidden;
        auto const panel = std::array { WindowFacts { visibility, WindowTransience::Independent } };
        CHECK(anyWindowKeepsAppAlive(panel) == row.keeps);
    }
}
