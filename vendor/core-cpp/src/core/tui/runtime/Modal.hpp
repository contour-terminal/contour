// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Coroutine-driven modal interactions for the TUI.
///
/// A @c ModalComponent is a self-contained interaction (a question, a picker, a
/// confirmation) that the runtime drives to a single @c Result. `runModal`
/// turns the hand-rolled "loop polling input until the prompt resolves" pattern
/// — duplicated across the agent auth flow, the AskUser prompt, and the
/// permission prompt — into one awaitable:
///
/// @code
/// auto answer = co_await runModal(runtime, question, &screen);
/// @endcode

#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/runtime/ModalComponent.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>

#include <optional>

namespace core::tui::runtime
{

/// Drives @p modal to completion on @p runtime, returning its result.
///
/// Each event is dispatched through @p screen first (so hover, tooltips, and
/// focus bookkeeping keep working) and then offered to `modal.step()`; the
/// screen is redrawn after every event the modal consumes. If the flow is
/// cancelled while awaiting input (Ctrl+C), the modal ends with std::nullopt,
/// matching the existing "Escape / cancelled" behavior.
///
/// The caller is responsible for placing @p modal in the screen tree (or as an
/// overlay) and for focus, before awaiting; `runModal` only drives the
/// interaction. Arguments are passed by pointer (not reference) because they are
/// coroutine parameters that outlive the suspension: the pointee must stay alive
/// for the whole call (see cppcoreguidelines-avoid-reference-coroutine-parameters).
///
/// @param runtime The runtime whose input the modal consumes (must outlive the call).
/// @param modal The interaction to drive (must outlive the call).
/// @param screen Optional screen to dispatch events through and redraw.
/// @return The modal's result, or std::nullopt if cancelled.
template <typename Result>
[[nodiscard]] core::async::Task<std::optional<Result>> runModal(TuiRuntime* runtime,
                                                                ModalComponent<Result>* modal,
                                                                Screen* screen = nullptr)
{
    if (screen != nullptr)
        screen->draw();

    while (true)
    {
        InputEvent event;
        try
        {
            event = co_await runtime->nextEvent();
        }
        catch (core::async::OperationCancelled const&)
        {
            co_return std::nullopt;
        }

        if (screen != nullptr)
            (void) screen->dispatchEvent(event);

        if (auto result = modal->step(event))
            co_return std::move(result);

        // Redraw only when the modal actually changed state, so ignored events
        // (e.g. mouse moves the modal does not act on) do not force a full redraw.
        if (screen != nullptr && modal->stepChangedState())
            screen->draw();
    }
}

} // namespace core::tui::runtime
