// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/GlobalHotkey.hpp>

#ifdef _WIN32

    #include <QtCore/QAbstractNativeEventFilter>

    #include <cstdint>

namespace contour::platform
{

/// The Windows global hotkey: RegisterHotKey with no window, so WM_HOTKEY is posted to the GUI thread's
/// message queue and reaches the application's native event filter.
///
/// Built before a QGuiApplication exists; the filter is installed at the first registration, and under any
/// platform plugin but the native Windows one a registration is refused as Unsupported, so the offscreen
/// plugin never touches the desktop's hot key table.
class Win32GlobalHotkey final: public IGlobalHotkey, public QAbstractNativeEventFilter
{
  public:
    Win32GlobalHotkey() = default;

    /// Releases the hot key and removes the filter.
    /// @pre GUI thread.
    ~Win32GlobalHotkey() override;

    /// @pre GUI thread.
    [[nodiscard]] HotkeyResult registerHotkey(config::HotkeyChord const& chord,
                                              HotkeyPressed onPressed,
                                              HotkeyResultCallback onResult) override;

    /// @pre GUI thread.
    void unregisterHotkey() override;

    /// @return The id the registered chord's WM_HOTKEY carries in its wParam, or 0 when none is registered.
    [[nodiscard]] int registeredId() const noexcept { return _id; }

    /// Answers WM_HOTKEY for the registered id by calling the pressed callback.
    bool nativeEventFilter(QByteArray const& eventType, void* message, qintptr* result) override;

  private:
    /// Whether this object's filter is installed on the application.
    enum class FilterState : uint8_t
    {
        Absent = 0,
        Installed,
    };

    int _id = 0;
    FilterState _filter = FilterState::Absent;
    HotkeyPressed _onPressed {};
};

} // namespace contour::platform

#endif
