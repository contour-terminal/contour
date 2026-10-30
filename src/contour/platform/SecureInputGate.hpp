// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one switch for the OS's secure keyboard entry. Qt-free.

#include <contour/platform/SecureInput.hpp>

namespace contour::platform
{

/// Follows a desired secure-input state and tells the OS only of changes: the only caller of ISecureInput,
/// so its calls are balanced by construction. enable() is called on Off -> On alone, disable() on On -> Off
/// alone, and the destructor turns an On state off, so no path that destroys the gate -- a closed window, a
/// torn-down manager, the app quitting -- leaves secure input on. Lives on the GUI thread, and is called
/// only there.
class SecureInputGate
{
  public:
    /// @param os The OS's secure keyboard entry; it must outlive the gate. Starts Off and calls nothing.
    explicit SecureInputGate(ISecureInput& os) noexcept: _os { os } {}

    SecureInputGate(SecureInputGate const&) = delete;
    SecureInputGate& operator=(SecureInputGate const&) = delete;
    SecureInputGate(SecureInputGate&&) = delete;
    SecureInputGate& operator=(SecureInputGate&&) = delete;

    /// Turns secure input off if it is on.
    /// @pre GUI (main) thread.
    ~SecureInputGate() { apply(SecureInputState::Off); }

    /// Moves to @p desired, calling the OS only when that is a change.
    /// @pre GUI (main) thread.
    /// @param desired The state secure input should be in.
    void apply(SecureInputState desired)
    {
        if (desired == _state)
            return;
        switch (desired)
        {
            case SecureInputState::On: _os.enable(); break;
            case SecureInputState::Off: _os.disable(); break;
        }
        _state = desired;
    }

    /// @return The state the OS was last moved to.
    [[nodiscard]] SecureInputState state() const noexcept { return _state; }

  private:
    ISecureInput& _os;
    SecureInputState _state = SecureInputState::Off;
};

} // namespace contour::platform
