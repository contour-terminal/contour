// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/SecureInput.hpp>

#ifdef __APPLE__

    #include <core/Assert.hpp>

    #include <memory>

    #import <Carbon/Carbon.h>
    #import <Foundation/Foundation.h>

namespace contour::platform
{

namespace
{
    /// macOS Secure Keyboard Entry: EnableSecureEventInput() and DisableSecureEventInput(), which the OS
    /// counts per process, so every enable must be matched by one disable. The gate keeps them balanced; this
    /// class keeps its own state as a second line of defence, so it never calls either twice in a row, and
    /// its destructor turns secure input off if the gate somehow did not. The OS ends a process's secure
    /// input when the process exits, which covers a crash.
    class CocoaSecureInput final: public ISecureInput
    {
      public:
        CocoaSecureInput() = default;
        CocoaSecureInput(CocoaSecureInput const&) = delete;
        CocoaSecureInput& operator=(CocoaSecureInput const&) = delete;
        CocoaSecureInput(CocoaSecureInput&&) = delete;
        CocoaSecureInput& operator=(CocoaSecureInput&&) = delete;

        /// Turns secure input off if it is on.
        /// @pre GUI (main) thread.
        ~CocoaSecureInput() override
        {
            if (_state == SecureInputState::On)
                disable();
        }

        /// @pre GUI (main) thread; currently off.
        void enable() override
        {
            Require(NSThread.isMainThread);
            if (_state == SecureInputState::On)
                return;
            EnableSecureEventInput();
            _state = SecureInputState::On;
        }

        /// @pre GUI (main) thread; currently on.
        void disable() override
        {
            Require(NSThread.isMainThread);
            if (_state == SecureInputState::Off)
                return;
            DisableSecureEventInput();
            _state = SecureInputState::Off;
        }

        /// @return Native.
        [[nodiscard]] SecureInputSupport support() const noexcept override
        {
            return SecureInputSupport::Native;
        }

      private:
        SecureInputState _state = SecureInputState::Off;
    };

} // namespace

std::unique_ptr<ISecureInput> makeSecureInput()
{
    return std::make_unique<CocoaSecureInput>();
}

} // namespace contour::platform

#endif
