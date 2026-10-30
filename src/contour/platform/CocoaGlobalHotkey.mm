// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/GlobalHotkey.hpp>
#include <contour/platform/HotkeyKeys.hpp>

#ifdef __APPLE__

    #include <core/Assert.hpp>

    #include <algorithm>
    #include <memory>
    #include <utility>

    #import <Carbon/Carbon.h>
    #import <Foundation/Foundation.h>

namespace contour::platform
{

namespace
{
    /// The signature and id the hot key is registered under; one hot key per process, so one id.
    constexpr auto HotkeySignature = static_cast<OSType>('CNTR');
    constexpr auto HotkeyId = static_cast<UInt32>(1);

    /// macOS global hotkey through Carbon's RegisterEventHotKey, which needs no accessibility permission.
    /// One kEventHotKeyPressed handler is installed on the application event target at the first
    /// registration and stays until the destructor; the hot key itself is released by unregisterHotkey().
    /// Carbon delivers the event on the main thread, so onPressed runs on the GUI thread.
    class CocoaGlobalHotkey final: public IGlobalHotkey
    {
      public:
        CocoaGlobalHotkey() = default;

        /// Releases the hot key and removes the handler.
        /// @pre GUI (main) thread.
        ~CocoaGlobalHotkey() override
        {
            unregisterHotkey();
            if (_handler != nullptr)
                RemoveEventHandler(_handler);
        }

        /// @pre GUI (main) thread.
        [[nodiscard]] HotkeyResult registerHotkey(config::HotkeyChord const& chord,
                                                  HotkeyPressed onPressed,
                                                  HotkeyResultCallback onResult) override
        {
            Require(NSThread.isMainThread);
            auto const result = bind(chord);
            if (result)
                _onPressed = std::move(onPressed);
            if (onResult)
                onResult(result);
            return result;
        }

        /// @pre GUI (main) thread.
        void unregisterHotkey() override
        {
            Require(NSThread.isMainThread);
            if (_hotKey != nullptr)
                UnregisterEventHotKey(_hotKey);
            _hotKey = nullptr;
            _onPressed = nullptr;
        }

      private:
        [[nodiscard]] HotkeyResult bind(config::HotkeyChord const& chord)
        {
            auto const* const keyRow = hotkeyKeyRow(chord.key);
            if (keyRow == nullptr)
                return std::unexpected(HotkeyError::UnsupportedKey);

            if (auto const installed = installHandler(); !installed)
                return installed;

            auto mask = UInt32 { 0 };
            for (auto const& modifierRow: HotkeyModifierRows)
                if (chord.modifiers.contains(modifierRow.modifier))
                    mask |= modifierRow.macMask;

            // The new chord is registered before the old one is released, so a refusal leaves the old one
            // bound.
            auto next = EventHotKeyRef { nullptr };
            auto const status =
                RegisterEventHotKey(keyRow->macVirtualKey,
                                    mask,
                                    EventHotKeyID { .signature = HotkeySignature, .id = HotkeyId },
                                    GetApplicationEventTarget(),
                                    0,
                                    &next);
            if (status == eventHotKeyExistsErr)
                return std::unexpected(HotkeyError::AlreadyTaken);
            if (status != noErr)
                return std::unexpected(HotkeyError::Failed);

            if (_hotKey != nullptr)
                UnregisterEventHotKey(_hotKey);
            _hotKey = next;
            return {};
        }

        [[nodiscard]] HotkeyResult installHandler()
        {
            if (_handler != nullptr)
                return {};
            auto const spec =
                EventTypeSpec { .eventClass = kEventClassKeyboard, .eventKind = kEventHotKeyPressed };
            auto const status =
                InstallApplicationEventHandler(&CocoaGlobalHotkey::onEvent, 1, &spec, this, &_handler);
            if (status != noErr)
            {
                _handler = nullptr;
                return std::unexpected(HotkeyError::Failed);
            }
            return {};
        }

        /// The Carbon callback; runs on the main thread.
        static OSStatus onEvent(EventHandlerCallRef /*next*/, EventRef event, void* userData)
        {
            auto id = EventHotKeyID {};
            auto const status = GetEventParameter(
                event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(id), nullptr, &id);
            if (status != noErr || id.signature != HotkeySignature || id.id != HotkeyId)
                return eventNotHandledErr;
            auto& self = *static_cast<CocoaGlobalHotkey*>(userData);
            if (self._onPressed)
                self._onPressed({});
            return noErr;
        }

        EventHotKeyRef _hotKey = nullptr;
        EventHandlerRef _handler = nullptr;
        HotkeyPressed _onPressed {};
    };

} // namespace

std::unique_ptr<IGlobalHotkey> makeGlobalHotkey()
{
    return std::make_unique<CocoaGlobalHotkey>();
}

} // namespace contour::platform

#endif
