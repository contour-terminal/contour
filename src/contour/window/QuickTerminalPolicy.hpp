// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/QuickTerminalConfig.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace contour::window
{

/// How much of the screen's available height the quick terminal takes.
inline constexpr double QuickTerminalHeightFraction = 0.4;

/// A rectangle in screen pixels.
struct ScreenRect
{
    int x = 0;      ///< Left edge.
    int y = 0;      ///< Top edge.
    int width = 0;  ///< Width.
    int height = 0; ///< Height.

    [[nodiscard]] bool operator==(ScreenRect const&) const = default;
};

/// Whether two rectangles share any area; rectangles that merely touch do not.
/// @param a One rectangle.
/// @param b The other.
/// @return Whether they overlap.
[[nodiscard]] constexpr bool overlaps(ScreenRect const& a, ScreenRect const& b) noexcept
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

/// Where the quick terminal sits shown, and where it waits hidden.
struct QuickTerminalFrames
{
    ScreenRect shown {};  ///< Full width, QuickTerminalHeightFraction of the height, flush with the top.
    ScreenRect hidden {}; ///< The shown frame moved up by its own height: just off the screen.
};

/// Derives both frames of the quick terminal from a screen's available geometry (below its menu bar).
/// @param available The screen's available geometry.
/// @return The shown and the hidden frame.
[[nodiscard]] constexpr QuickTerminalFrames quickTerminalFrames(ScreenRect available) noexcept
{
    auto const exact = static_cast<double>(available.height) * QuickTerminalHeightFraction;
    auto const whole = static_cast<int>(exact);
    auto const height = (exact - static_cast<double>(whole)) >= 0.5 ? whole + 1 : whole;
    auto const shown =
        ScreenRect { .x = available.x, .y = available.y, .width = available.width, .height = height };
    auto hidden = shown;
    hidden.y -= shown.height;
    return QuickTerminalFrames { .shown = shown, .hidden = hidden };
}

/// Whether the quick terminal's hidden frame, where a slide starts and ends, is out of every other screen's
/// sight: it lies above its screen, so a screen stacked above would show it.
enum class SlideClearance : uint8_t
{
    Blocked = 0, ///< The hidden frame overlaps another screen.
    Clear,       ///< The hidden frame lies off every screen.
};

/// Whether a window parked at the hidden frame would be seen on another screen.
/// @param frames       The frames derived from the target screen's available geometry.
/// @param otherScreens The full geometry of every screen but the target.
/// @return Blocked when the hidden frame overlaps one of them.
[[nodiscard]] constexpr SlideClearance quickTerminalSlideClearance(
    QuickTerminalFrames const& frames, std::span<ScreenRect const> otherScreens) noexcept
{
    for (auto const& screen: otherScreens)
        if (overlaps(frames.hidden, screen))
            return SlideClearance::Blocked;
    return SlideClearance::Clear;
}

/// Where the quick terminal's life stands.
enum class QuickTerminalState : uint8_t
{
    Absent = 0, ///< No window exists.
    Hidden,     ///< The window exists, off the screen.
    Showing,    ///< Sliding or fading in.
    Shown,      ///< On the screen.
    Hiding,     ///< Sliding or fading out.
};

/// @param state Where the quick terminal's life stands.
/// @return Whether its window is on the screen, or on its way on or off it, so that it gives the app a reason
///         to keep running. A hidden quick terminal gives none (ruling D1): it is never what keeps the
///         process alive, whether or not the platform still counts its parked window as visible.
[[nodiscard]] constexpr bool quickTerminalKeepsAppAlive(QuickTerminalState state) noexcept
{
    switch (state)
    {
        case QuickTerminalState::Showing:
        case QuickTerminalState::Shown:
        case QuickTerminalState::Hiding: return true;
        case QuickTerminalState::Absent:
        case QuickTerminalState::Hidden: break;
    }
    return false;
}

/// What happens to the quick terminal.
enum class QuickTerminalEvent : uint8_t
{
    Hotkey = 0,    ///< The global hotkey was pressed.
    FocusLost,     ///< The window lost the focus.
    FocusGained,   ///< The window gained the focus.
    AnimationDone, ///< The slide or fade ended.
    LastTabClosed, ///< The window's last tab closed, so the window is gone.
};

/// Whether the quick terminal's window has the keyboard focus.
enum class QuickTerminalFocus : uint8_t
{
    Unfocused = 0, ///< Another window has it.
    Focused,       ///< The quick terminal has it.
};

/// What the controller does for a transition.
enum class QuickTerminalEffect : uint8_t
{
    None = 0,      ///< Nothing.
    CreateAndShow, ///< Make the window, then show it.
    Show,          ///< Show the existing window.
    Hide,          ///< Hide the window.
    Focus,         ///< Give the shown window the focus.
    Reverse,       ///< Run the running animation backwards.
    Destroy,       ///< Forget the window.
};

/// One row of the quick terminal's transition table.
struct QuickTerminalTransition
{
    QuickTerminalState from {};                  ///< The state it applies in.
    QuickTerminalEvent event {};                 ///< The event it answers.
    QuickTerminalFocus focus {};                 ///< The focus the window has when the event comes.
    config::QuickTerminalFocusLoss focusLoss {}; ///< The configured reaction to losing the focus.
    QuickTerminalState to {};                    ///< The state it leads to.
    QuickTerminalEffect effect {};               ///< What to do on the way.
};

namespace detail
{
    /// A transition written once for every focus and focus-loss value it does not name.
    struct TransitionPattern
    {
        std::optional<QuickTerminalState> from {};                  ///< Absent: every state but Absent.
        QuickTerminalEvent event {};                                ///< The event.
        std::optional<QuickTerminalFocus> focus {};                 ///< Absent: either.
        std::optional<config::QuickTerminalFocusLoss> focusLoss {}; ///< Absent: either.
        QuickTerminalState to {};                                   ///< The state it leads to.
        QuickTerminalEffect effect {};                              ///< What to do.
    };

    inline constexpr auto FocusValues =
        std::array { QuickTerminalFocus::Unfocused, QuickTerminalFocus::Focused };
    inline constexpr auto FocusLossValues =
        std::array { config::QuickTerminalFocusLoss::Hide, config::QuickTerminalFocusLoss::Stay };

    inline constexpr auto AllStates = std::array { QuickTerminalState::Absent,
                                                   QuickTerminalState::Hidden,
                                                   QuickTerminalState::Showing,
                                                   QuickTerminalState::Shown,
                                                   QuickTerminalState::Hiding };

    using S = QuickTerminalState;
    using E = QuickTerminalEvent;
    using F = QuickTerminalFocus;
    using L = config::QuickTerminalFocusLoss;
    using X = QuickTerminalEffect;

    /// The transitions, as written down; QuickTerminalTransitions is this with the wildcards expanded.
    inline constexpr auto TransitionPatterns = std::array {
        TransitionPattern { S::Absent, E::Hotkey, std::nullopt, std::nullopt, S::Showing, X::CreateAndShow },
        TransitionPattern { S::Hidden, E::Hotkey, std::nullopt, std::nullopt, S::Showing, X::Show },
        TransitionPattern { S::Showing, E::AnimationDone, std::nullopt, std::nullopt, S::Shown, X::None },
        TransitionPattern { S::Shown, E::Hotkey, F::Focused, std::nullopt, S::Hiding, X::Hide },
        TransitionPattern { S::Shown, E::Hotkey, F::Unfocused, std::nullopt, S::Shown, X::Focus },
        TransitionPattern { S::Shown, E::FocusLost, std::nullopt, L::Hide, S::Hiding, X::Hide },
        TransitionPattern { S::Shown, E::FocusLost, std::nullopt, L::Stay, S::Shown, X::None },
        TransitionPattern { S::Showing, E::FocusLost, std::nullopt, L::Hide, S::Hiding, X::Reverse },
        TransitionPattern { S::Showing, E::Hotkey, std::nullopt, std::nullopt, S::Hiding, X::Reverse },
        TransitionPattern { S::Hiding, E::Hotkey, std::nullopt, std::nullopt, S::Showing, X::Reverse },
        TransitionPattern { S::Hiding, E::AnimationDone, std::nullopt, std::nullopt, S::Hidden, X::None },
        TransitionPattern {
            std::nullopt, E::LastTabClosed, std::nullopt, std::nullopt, S::Absent, X::Destroy },
    };

    /// Whether @p pattern applies to a state, as the expansion sees it.
    [[nodiscard]] constexpr bool patternCovers(TransitionPattern const& pattern,
                                               QuickTerminalState state) noexcept
    {
        return pattern.from ? *pattern.from == state : state != QuickTerminalState::Absent;
    }

    /// How many rows the expansion of TransitionPatterns has.
    [[nodiscard]] constexpr size_t expandedTransitionCount() noexcept
    {
        auto count = size_t { 0 };
        for (auto const& pattern: TransitionPatterns)
            for (auto const stateValue: AllStates)
                if (patternCovers(pattern, stateValue))
                    count += (pattern.focus ? 1 : FocusValues.size())
                             * (pattern.focusLoss ? 1 : FocusLossValues.size());
        return count;
    }

    /// Expands TransitionPatterns into one row per state, focus and focus-loss value it covers.
    [[nodiscard]] constexpr auto expandTransitions() noexcept
    {
        auto rows = std::array<QuickTerminalTransition, expandedTransitionCount()> {};
        auto next = size_t { 0 };
        for (auto const& pattern: TransitionPatterns)
            for (auto const stateValue: AllStates)
                if (patternCovers(pattern, stateValue))
                    for (auto const focus: FocusValues)
                        for (auto const focusLoss: FocusLossValues)
                            if ((!pattern.focus || *pattern.focus == focus)
                                && (!pattern.focusLoss || *pattern.focusLoss == focusLoss))
                                rows[next++] = QuickTerminalTransition { .from = stateValue,
                                                                         .event = pattern.event,
                                                                         .focus = focus,
                                                                         .focusLoss = focusLoss,
                                                                         .to = pattern.to,
                                                                         .effect = pattern.effect };
        return rows;
    }
} // namespace detail

/// Every transition, one row per combination of state, event, focus and focus-loss setting; an event
/// that has no row in a state is ignored.
inline constexpr auto QuickTerminalTransitions = detail::expandTransitions();

/// Looks up what an event does in a state.
/// @param from      The current state.
/// @param event     What happened.
/// @param focus     Whether the window has the focus.
/// @param focusLoss The configured reaction to losing the focus.
/// @return The transition, or nullopt when the event is ignored.
[[nodiscard]] constexpr std::optional<QuickTerminalTransition> quickTerminalStep(
    QuickTerminalState from,
    QuickTerminalEvent event,
    QuickTerminalFocus focus,
    config::QuickTerminalFocusLoss focusLoss) noexcept
{
    for (auto const& row: QuickTerminalTransitions)
        if (row.from == from && row.event == event && row.focus == focus && row.focusLoss == focusLoss)
            return row;
    return std::nullopt;
}

/// What the window does to appear and disappear.
enum class QuickTerminalMotion : uint8_t
{
    Slide = 0, ///< Travels from above the screen.
    Fade,      ///< Changes opacity in place.
};

/// Who decides where a window sits.
enum class PlacementControl : uint8_t
{
    Client = 0, ///< The application places its windows.
    Compositor, ///< Only the compositor does (Wayland), so there is nothing to slide.
};

/// The appearance animation of the quick terminal.
struct QuickTerminalAnimation
{
    QuickTerminalMotion motion {};         ///< Slide or fade.
    std::chrono::milliseconds duration {}; ///< How long it takes.
};

/// Picks the animation: a slide where the window may travel without crossing another screen and motion is
/// on, a fade otherwise.
/// @param motion    The motion tokens; their emphasized duration is already the cross-fade's under Reduce
/// Motion.
/// @param placement Who places the window.
/// @param clearance Whether the hidden frame is off every other screen.
/// @return The motion and its duration.
[[nodiscard]] constexpr QuickTerminalAnimation quickTerminalAnimation(config::MotionTokens const& motion,
                                                                      PlacementControl placement,
                                                                      SlideClearance clearance) noexcept
{
    auto const slides = motion.style == config::MotionStyle::Full && placement == PlacementControl::Client
                        && clearance == SlideClearance::Clear;
    return QuickTerminalAnimation { .motion = slides ? QuickTerminalMotion::Slide : QuickTerminalMotion::Fade,
                                    .duration = std::chrono::milliseconds(motion.emphasizedMs) };
}

} // namespace contour::window
