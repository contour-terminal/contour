// SPDX-License-Identifier: Apache-2.0
//
// TerminalSession's key input: executing the route the platform-free router planned for a key event.
// What a platform means by a key is decided below this file (contour::input::KeyboardPlatform), and which
// binding a chord reaches by the router (contour::input::KeyRouter); this file only runs the plan, with the
// session state around it -- an ended pane's footer, the echo watch, block focus, the shell probe.
//
// Kept apart from TerminalSession.cpp so that the whole file can be held free of platform conditionals.

#include <contour/config/Actions.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/input/KeyRouter.hpp>
#include <contour/input/Logging.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/input/HeldModifiers.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <core/Utils.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <variant>

using vtbackend::Key;
using vtbackend::KeyboardEventType;
using vtbackend::KeyboardModifiers;
using vtbackend::KeyIdentity;

namespace contour::session
{

namespace
{
    /// What became of a key or character event once its bindings were tried.
    enum class BindingOutcome : uint8_t
    {
        PassOn = 0, ///< No binding claimed it: it goes on to the next candidate, then to the application.
        Claimed,    ///< A binding ran, or swallowed it.
    };

    /// Runs a candidate's actions for a press or a repeat, through the repeat filter.
    ///
    /// A press whose actions all decline is claimed only when the candidate says so. A repeat whose actions
    /// were all filtered out is always claimed: the binding owns the chord, so its repeats must not reach
    /// the application as the chord's own encoding (a held Ctrl+Shift+G would send ^G per repeat).
    /// @param candidate The bound actions (none when nothing is bound) and what a declined press does.
    /// @param eventType Press or Repeat.
    /// @param execute Runs a list of actions, returning how many ran.
    /// @return Whether the event was claimed.
    template <typename Execute>
    [[nodiscard]] BindingOutcome runBinding(input::BindingCandidate const& candidate,
                                            KeyboardEventType eventType,
                                            Execute const& execute)
    {
        auto const* actions = candidate.actions;
        if (actions == nullptr)
            return BindingOutcome::PassOn;

        // Drop actions that must not fire on auto-repeat (e.g. CloseTab/ClosePane/CreateNewTab). The
        // overwhelmingly common held keys (characters, arrows) bind only repeatable actions, so avoid
        // allocating a filtered copy unless there is actually a non-repeatable action to drop.
        auto executed = 0;
        if (eventType != KeyboardEventType::Repeat
            || std::ranges::none_of(*actions, actions::isNonRepeatable))
            executed = execute(*actions);
        else if (auto const repeatable = actions::filterRepeatableActions(*actions); repeatable.empty())
            return BindingOutcome::Claimed;
        else
            executed = execute(repeatable);

        return executed > 0 || candidate.declined == config::DeclinedInput::Consume ? BindingOutcome::Claimed
                                                                                    : BindingOutcome::PassOn;
    }

    /// Walks a route's binding candidates in order: the user's row, then the built-in one, which a user
    /// binding that declines lets try.
    /// @return Whether a candidate claimed the event.
    template <typename Execute>
    [[nodiscard]] BindingOutcome runCandidates(input::KeyRoute const& route,
                                               KeyboardEventType eventType,
                                               Execute const& execute)
    {
        if (runBinding(route.user, eventType, execute) == BindingOutcome::Claimed)
            return BindingOutcome::Claimed;
        return runBinding(route.fallback, eventType, execute);
    }
} // namespace

input::RouteContext TerminalSession::keyRouteContext() const
{
    // Both read without the state lock, as a key must not wait for the parser thread's batch: the binding
    // modes are published atomically, and the keyboard protocol is read under the input lock.
    return input::RouteContext {
        .config = _config,
        .modes = _terminal.bindingModes(),
        .superEncoding = _terminal.isKeyboardProtocolNonLegacy() ? input::SuperEncoding::Representable
                                                                 : input::SuperEncoding::Unrepresentable,
    };
}

KeyDelivery TerminalSession::sendKeyboardEvent(input::KeyEvent const& event, Timestamp now)
{
    if (event.key)
    {
        deliverKey(*event.key, event.modifiers, event.appModifier, event.type, now);
        return KeyDelivery::Handled;
    }
    if (event.text.empty())
        return KeyDelivery::Unhandled;
    for (char32_t const ch: event.text)
        deliverChar(ch, event.identity, event.modifiers, event.appModifier, event.type, now);
    return KeyDelivery::Handled;
}

void TerminalSession::sendKeyEvent(Key key,
                                   KeyboardModifiers modifiers,
                                   KeyboardEventType eventType,
                                   Timestamp now)
{
    deliverKey(key, modifiers, input::AppModifier::Absent, eventType, now);
}

void TerminalSession::sendCharEvent(char32_t value,
                                    KeyIdentity keyIdentity,
                                    KeyboardModifiers modifiers,
                                    KeyboardEventType eventType,
                                    Timestamp now)
{
    deliverChar(value, keyIdentity, modifiers, input::AppModifier::Absent, eventType, now);
}

void TerminalSession::deliverKey(Key key,
                                 KeyboardModifiers modifiers,
                                 input::AppModifier appModifier,
                                 KeyboardEventType eventType,
                                 Timestamp now)
{
    input::inputLog()("Key {} event received: {} {}", eventType, modifiers, key);

    // An ended pane waits for its footer's answer: the input mappings still run first (so the ClosePane
    // binding closes it, through terminate()'s already-closed branch, and Alt+Enter still toggles full
    // screen), an unmodified Return they leave alone is Restart, and nothing reaches the PTY, whose
    // program is gone. GUI-thread state, read without the lock (see _end).
    auto const ended = _end != EndReason::None;

    auto const modifierOnly = vtbackend::isModifierKey(key);
    if (modifierOnly)
        onHeldModifiersChanged(vtbackend::heldAfter(key, eventType, modifiers.chord));

    // A bare modifier is no typing: hiding the pointer then hides the very link Ctrl is held to reach.
    // Guarded for display-less sessions (background pane, headless test), which have no cursor to hide.
    if (_profile.mouse.value().hideWhileTyping && _display != nullptr && !modifierOnly)
        _display->setMouseCursorShape(input::MouseCursorShape::Hidden);

    // A release is not routed: it runs no binding, and goes to the terminal as it came.
    if (eventType != KeyboardEventType::Release)
    {
        auto const route = input::routeKey(key, modifiers, appModifier, keyRouteContext());
        auto const execute = [this](auto const& actions) {
            return executeAllActions(actions);
        };
        if (runCandidates(route, eventType, execute) == BindingOutcome::Claimed)
            return;
        if (std::holds_alternative<input::ConsumeInput>(route.otherwise))
            return;
    }
    if (ended)
    {
        // Only a bare Return: a modified one is a chord the user may bind, and a release or repeat is
        // not a request. The Return's release then reaches the NEW session, whose terminal ignores it:
        // a fresh terminal has negotiated no keyboard protocol that reports releases.
        if (eventType == KeyboardEventType::Press && (key == Key::Enter || key == Key::Numpad_Enter)
            && modifiers.chord.none())
            runFooterAction();
        return;
    }
    // Before the key leaves: a prompt that turned echo off after its last output (stty -echo; read) is
    // noticed here. This key has already been shown to the OS (event taps saw it before Qt did), so secure
    // input protects the keys after it, not this one; it also releases a stale lock before the key goes in.
    refreshInputEcho();
    noteInputActivity();
    // Typing is the keyboard acting on the shell, not on a block. A bare modifier is no typing: Ctrl goes
    // down before the Ctrl+Enter that needs the focus, and a release ends nothing.
    if (eventType != KeyboardEventType::Release && !modifierOnly)
        dropBlockFocusOnInput();
    if (eventType == KeyboardEventType::Press && (key == Key::Enter || key == Key::Numpad_Enter)
        && modifiers.chord.none())
        noteCommandEntered();
    terminal().sendKeyEvent(key, modifiers, eventType, now);
    // Echo turned back on with nothing printed (a prompt answered by a program that stays quiet) is
    // noticed at the next key, once the key's bytes have reached the PTY.
    refreshInputEcho();
}

void TerminalSession::deliverChar(char32_t value,
                                  KeyIdentity keyIdentity,
                                  KeyboardModifiers modifiers,
                                  input::AppModifier appModifier,
                                  KeyboardEventType eventType,
                                  Timestamp now)
{
    input::inputLog()("Character {} event received: {} '{}'",
                      eventType,
                      modifiers,
                      core::escape(unicode::convert_to<char>(value)));

    // As in deliverKey: an ended pane runs the input mappings, and passes no text to the PTY.
    auto const ended = _end != EndReason::None;

    if (_profile.mouse.value().hideWhileTyping && _display != nullptr)
        _display->setMouseCursorShape(input::MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        auto const route = input::routeChar(value, keyIdentity, modifiers, appModifier, keyRouteContext());
        auto const execute = [this](auto const& actions) {
            return executeAllActions(actions);
        };
        if (runCandidates(route, eventType, execute) == BindingOutcome::Claimed)
            return;
        // A character the terminal cannot represent, which no binding wanted: nothing is written.
        if (std::holds_alternative<input::ConsumeInput>(route.otherwise))
            return;
    }
    if (ended)
        return;
    refreshInputEcho(); // before the key leaves, as in deliverKey
    noteInputActivity();
    if (eventType != KeyboardEventType::Release)
        dropBlockFocusOnInput();
    terminal().sendCharEvent(value, keyIdentity, modifiers, eventType, now);
    refreshInputEcho(); // as in deliverKey
}

} // namespace contour::session
