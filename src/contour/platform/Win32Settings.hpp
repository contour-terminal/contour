// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/platform/WindowsAppearanceSources.hpp>
#include <contour/platform/WindowsSettings.hpp>

#ifdef _WIN32

    #include <QtCore/QAbstractNativeEventFilter>
    #include <QtCore/QCoreApplication>
    #include <QtCore/QPointer>

    #include <functional>
    #include <memory>
    #include <utility>

namespace contour::platform
{

/// Reads Windows' settings: three DWORDs from HKEY_CURRENT_USER, two SystemParametersInfoW answers and the
/// build from RtlGetVersion (resolved from ntdll.dll at run time, since GetVersionEx lies without a
/// manifest). A failed read leaves its optional empty or its enum at the default.
class Win32SettingsReader final: public IWindowsSettingsReader
{
  public:
    /// @pre GUI thread.
    /// @return The settings as Windows reports them now.
    [[nodiscard]] WindowsSettingsReadings read() const override;
};

/// @return Whether the running platform plugin is the native Windows one. Asked on every call rather than at
///         construction, which precedes the application (F1).
[[nodiscard]] bool onNativeWindowsPlatform();

/// Decodes WM_SETTINGCHANGE, WM_DWMCOLORIZATIONCOLORCHANGED and WM_THEMECHANGED into a SettingChange and
/// reports its topic (@ref topicOf) when it concerns any source. Never consumes the message.
///
/// A broadcast reaches every top-level window, so one change may be reported once per Contour window; the
/// sources report only a value that differs, so that costs a re-read, not a repeated change.
class Win32SettingsWatcher final: public QAbstractNativeEventFilter
{
  public:
    /// Installs the filter if an application exists already; @ref installIfPossible does it later otherwise.
    /// @param onTopic Called on the GUI thread with each broadcast's topic other than None.
    explicit Win32SettingsWatcher(std::function<void(SettingTopic)> onTopic);

    Win32SettingsWatcher(Win32SettingsWatcher const&) = delete;
    Win32SettingsWatcher& operator=(Win32SettingsWatcher const&) = delete;
    Win32SettingsWatcher(Win32SettingsWatcher&&) = delete;
    Win32SettingsWatcher& operator=(Win32SettingsWatcher&&) = delete;

    /// Removes the filter from the application it was installed on, if that still exists.
    ~Win32SettingsWatcher() override;

    /// Installs the filter on the application under the native Windows platform, once.
    /// @pre GUI thread.
    void installIfPossible();

    /// Qt's hook; the signature is Qt's, so its bool (whether the message was consumed) stays.
    /// @return false: the message always continues to its window.
    bool nativeEventFilter(QByteArray const& eventType, void* message, qintptr* result) override;

  private:
    std::function<void(SettingTopic)> _onTopic;
    QPointer<QCoreApplication> _installedOn {}; ///< The application filtered, or null before it is installed.
};

/// One of Task 12's Windows sources, followed by a broadcast watcher it owns. Off the native Windows platform
/// it reports the defaults, as the null source does.
/// @tparam Interface The source interface implemented.
/// @tparam Source The Windows source answering it.
template <typename Interface, typename Source>
class Win32WatchedSource final: public Interface
{
  public:
    /// What the source reports.
    using Value = decltype(std::declval<Source const&>().current());

    /// @param reader The settings reader.
    /// @param onChange Called on the GUI thread with each changed value; may be empty.
    Win32WatchedSource(std::shared_ptr<IWindowsSettingsReader const> reader,
                       std::function<void(Value)> onChange):
        _source { std::move(reader), std::move(onChange) },
        _watcher { [this](SettingTopic topic) { _source.settingsChanged(topic); } }
    {
    }

    Win32WatchedSource(Win32WatchedSource const&) = delete;
    Win32WatchedSource& operator=(Win32WatchedSource const&) = delete;
    Win32WatchedSource(Win32WatchedSource&&) = delete;
    Win32WatchedSource& operator=(Win32WatchedSource&&) = delete;
    ~Win32WatchedSource() override = default;

    /// Installs the watcher once an application exists: the source was built before it.
    /// @pre GUI thread.
    /// @return The value a fresh read means, or the defaults off the native Windows platform.
    [[nodiscard]] Value current() const override
    {
        if (!onNativeWindowsPlatform())
            return Value {};
        _watcher.installIfPossible();
        return _source.current();
    }

  private:
    Source _source;
    /// Mutable: installing the filter lazily from current() is what F1's construction order requires.
    mutable Win32SettingsWatcher _watcher;
};

/// The Windows accessibility preference source the factory builds.
using Win32AccessibilityPreferenceSource =
    Win32WatchedSource<IAccessibilityPreferenceSource, WindowsAccessibilityPreferenceSource>;

/// The Windows platform appearance the factory builds.
using Win32PlatformAppearance = Win32WatchedSource<IPlatformAppearance, WindowsPlatformAppearance>;

} // namespace contour::platform

#endif
