// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/GlobalHotkey.hpp>
#include <contour/platform/Win32GlobalHotkey.hpp>

#ifdef _WIN32

    #include <contour/platform/HotkeyKeys.hpp>
    #include <contour/platform/Win32Settings.hpp>

    #include <QtCore/QByteArray>
    #include <QtCore/QCoreApplication>

    #include <atomic>
    #include <memory>
    #include <utility>

    #include <Windows.h>

namespace contour::platform
{

namespace
{
    /// Hot key ids are per thread and must differ between hot keys registered on it, so two hotkeys of one
    /// process (a Settings reload builds a second before the first is gone) draw from one counter.
    [[nodiscard]] int nextHotkeyId() noexcept
    {
        static auto counter = std::atomic<int> { 0 };
        return ++counter;
    }

    /// The event types under which Qt's Windows dispatcher hands out messages: window messages as
    /// "windows_generic_MSG", thread messages (a hot key registered with no window) as
    /// "windows_dispatcher_MSG".
    [[nodiscard]] bool isWindowsMessage(QByteArray const& eventType) noexcept
    {
        return eventType == "windows_dispatcher_MSG" || eventType == "windows_generic_MSG";
    }
} // namespace

Win32GlobalHotkey::~Win32GlobalHotkey()
{
    unregisterHotkey();
    if (_filter == FilterState::Installed)
        if (auto* const app = QCoreApplication::instance())
            app->removeNativeEventFilter(this);
}

HotkeyResult Win32GlobalHotkey::registerHotkey(config::HotkeyChord const& chord,
                                               HotkeyPressed onPressed,
                                               HotkeyResultCallback onResult)
{
    auto const result = [&]() -> HotkeyResult {
        auto const* const keyRow = hotkeyKeyRow(chord.key);
        if (keyRow == nullptr)
            return std::unexpected(HotkeyError::UnsupportedKey);
        if (!onNativeWindowsPlatform())
            return std::unexpected(HotkeyError::Unsupported);

        auto mask = static_cast<UINT>(MOD_NOREPEAT);
        for (auto const& modifierRow: HotkeyModifierRows)
            if (chord.modifiers.contains(modifierRow.modifier))
                mask |= modifierRow.windowsMask;

        // The new chord is registered before the old one is released, so a refusal leaves the old one bound.
        auto const id = nextHotkeyId();
        if (!RegisterHotKey(nullptr, id, mask, keyRow->windowsVirtualKey))
            return std::unexpected(GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED
                                       ? HotkeyError::AlreadyTaken
                                       : HotkeyError::Failed);

        if (_id != 0)
            UnregisterHotKey(nullptr, _id);
        _id = id;
        _onPressed = std::move(onPressed);
        if (_filter == FilterState::Absent)
        {
            QCoreApplication::instance()->installNativeEventFilter(this);
            _filter = FilterState::Installed;
        }
        return {};
    }();

    if (onResult)
        onResult(result);
    return result;
}

void Win32GlobalHotkey::unregisterHotkey()
{
    if (_id != 0)
        UnregisterHotKey(nullptr, _id);
    _id = 0;
    _onPressed = nullptr;
}

bool Win32GlobalHotkey::nativeEventFilter(QByteArray const& eventType, void* message, qintptr* result)
{
    if (_id == 0 || !isWindowsMessage(eventType))
        return false;
    auto const* const msg = static_cast<MSG const*>(message);
    if (msg->message != WM_HOTKEY || msg->wParam != static_cast<WPARAM>(_id))
        return false;
    if (_onPressed)
        _onPressed({});
    // Qt hands thread messages (windows_dispatcher_MSG, which a hot key with no window is) to the filter with
    // no result to fill in: only a window message has one.
    if (result != nullptr)
        *result = 0;
    return true;
}

std::unique_ptr<IGlobalHotkey> makeGlobalHotkey()
{
    return std::make_unique<Win32GlobalHotkey>();
}

} // namespace contour::platform

#endif
