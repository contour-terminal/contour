// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ModalComponent<Result>` — the contract a self-contained modal interaction
/// implements so the runtime can drive it to a single result.
///
/// This is a lightweight header (it does not pull in the coroutine runtime), so
/// concrete components such as @c QuestionComponent can derive from it without
/// depending on `Task` / `TuiRuntime`. The coroutine driver `runModal` that
/// actually advances a modal lives in `Modal.hpp`.

#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>

#include <optional>

namespace core::tui::runtime
{

/// A component that drives a self-contained modal interaction to completion.
///
/// `render()` stays synchronous (it runs every frame); the modal advances by
/// feeding events to `step()`, which returns a @c Result to finish or
/// std::nullopt to keep going. This keeps the existing per-frame render/dispatch
/// contract while making the *interaction* awaitable (via `runModal`).
/// @tparam Result The value the interaction yields when it completes.
template <typename Result>
class ModalComponent: public Component
{
  public:
    /// Feeds one input event to the interaction.
    /// @param event The event to process.
    /// @return The final result to end the modal, or std::nullopt to continue.
    [[nodiscard]] virtual std::optional<Result> step(InputEvent const& event) = 0;

    /// @return True if the most recent @c step() changed visible state and the
    ///         screen should be redrawn. Defaults to true (always redraw) to
    ///         preserve behavior for modals that do not track change state;
    ///         overrides gate redraws to events that actually changed something,
    ///         avoiding wasted draws on ignored events (e.g. mouse moves).
    [[nodiscard]] virtual bool stepChangedState() const noexcept { return true; }
};

} // namespace core::tui::runtime
