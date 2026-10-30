// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/TaskbarProgressRouter.hpp>

#include <optional>

namespace contour::platform
{

/// Orders Windows taskbar calls, stopping after the first failed call.
/// @param progress Desired aggregate.
/// @param setValue Writes a percentage and returns an expected result.
/// @param setState Writes a state and returns the same expected result type.
/// @return The complete update's result. Busy first clears any old determinate value.
template <typename SetValue, typename SetState>
[[nodiscard]] auto applyWindowsTaskbarProgress(TaskbarProgress progress, SetValue setValue, SetState setState)
{
    if (progress.state == vtbackend::ProgressState::Indeterminate)
    {
        if (auto result = setState(vtbackend::ProgressState::Inactive); !result)
            return result;
    }
    else if (progress.state != vtbackend::ProgressState::Inactive)
    {
        if (auto result = setValue(progress.percentage); !result)
            return result;
    }
    return setState(progress.state);
}

/// Pending versus successfully published state for one native taskbar button: one readiness rule for
/// everything the button can show.
/// @tparam Desired What the button is asked to show (@c TaskbarProgress, @c TaskbarAttention).
template <typename Desired>
class TaskbarButtonState
{
  public:
    /// Retains the latest desired value, whether or not the button exists yet.
    /// @param desired The value to publish.
    void set(Desired desired) noexcept { _desired = desired; }

    /// A new button needs the current value replayed, even if an older button already received it.
    void buttonCreated() noexcept
    {
        _button = Button::Ready;
        _sent.reset();
    }

    /// The same value must be sent again on the same button, because how it is drawn changed (the icon's
    /// scale after a screen or pixel-ratio change); a button that is not ready stays unready.
    void invalidate() noexcept { _sent.reset(); }

    /// A destroyed native surface must receive no more updates until its new button is created.
    void surfaceDestroyed() noexcept
    {
        _button = Button::Unavailable;
        _sent.reset();
    }

    /// Returns the next update, suppressing only updates that already succeeded on this button.
    /// @return The value to send, or nullopt while unavailable or unchanged.
    [[nodiscard]] std::optional<Desired> pending() const noexcept
    {
        return _button == Button::Ready && _sent != _desired ? std::optional { _desired } : std::nullopt;
    }

    /// Acknowledges a complete native update. Failures leave pending() unchanged for a later retry.
    void applied() noexcept { _sent = _desired; }

  private:
    enum class Button : uint8_t
    {
        Unavailable,
        Ready,
    };

    Button _button = Button::Unavailable;
    Desired _desired {};
    std::optional<Desired> _sent;
};

/// The taskbar button's progress, with the readiness rule of @c TaskbarButtonState.
using TaskbarProgressState = TaskbarButtonState<TaskbarProgress>;

/// The taskbar button's waiting-tab overlay, with the readiness rule of @c TaskbarButtonState.
using TaskbarAttentionState = TaskbarButtonState<TaskbarAttention>;

} // namespace contour::platform
