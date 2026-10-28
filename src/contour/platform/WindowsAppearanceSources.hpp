// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/platform/WindowsSettings.hpp>

#include <functional>
#include <memory>
#include <utility>

// The Windows accessibility and appearance sources: what WindowsSettings.hpp decides from a reader's
// readings, re-read when a settings broadcast concerns them. Compiled on every OS, driven by a fake reader in
// tests; the Win32 factories (Win32Settings.cpp) hand them the real reader and a broadcast watcher.
namespace contour::platform
{

namespace detail
{
    /// Maps a reader's readings to @p T, and reports a re-read value only when it differs from the last one
    /// reported. The shape both Windows sources share.
    template <typename T>
    class WindowsSettingReport
    {
      public:
        /// What the readings mean for this source.
        using Mapping = T (*)(WindowsSettingsReadings const&) noexcept;

        /// @param reader The settings reader; read once here for the value later changes compare against.
        /// @param mapping What the readings mean.
        /// @param topic The broadcast topic this source follows, Accessibility or Appearance.
        /// @param onChange Called with each changed value; may be empty.
        WindowsSettingReport(std::shared_ptr<IWindowsSettingsReader const> reader,
                             Mapping mapping,
                             SettingTopic topic,
                             std::function<void(T)> onChange):
            _reader { std::move(reader) },
            _mapping { mapping },
            _topic { topic },
            _onChange { std::move(onChange) },
            _reported { _mapping(_reader->read()) }
        {
        }

        /// @pre GUI thread.
        /// @return The value a fresh read means.
        [[nodiscard]] T current() const { return _mapping(_reader->read()); }

        /// Re-reads when @p topic concerns this source, and reports a changed value.
        /// @pre GUI thread.
        /// @param topic What a settings broadcast concerns.
        void settingsChanged(SettingTopic topic)
        {
            if (!topicConcerns(topic, _topic))
                return;
            auto next = current();
            if (next == _reported)
                return;
            _reported = next;
            if (_onChange)
                _onChange(std::move(next));
        }

      private:
        std::shared_ptr<IWindowsSettingsReader const> _reader;
        Mapping _mapping;
        SettingTopic _topic;
        std::function<void(T)> _onChange;
        T _reported; ///< The value last reported, or read at construction.
    };
} // namespace detail

/// Windows' accessibility preferences: animation effects, transparency effects and contrast themes.
class WindowsAccessibilityPreferenceSource final: public IAccessibilityPreferenceSource
{
  public:
    /// @param reader The settings reader, shared with the appearance source.
    /// @param onChange Called on the GUI thread with each changed value; may be empty.
    WindowsAccessibilityPreferenceSource(std::shared_ptr<IWindowsSettingsReader const> reader,
                                         AccessibilityPreferencesChanged onChange);

    /// @pre GUI thread.
    /// @return The preferences a fresh read means.
    [[nodiscard]] config::AccessibilityPreferences current() const override;

    /// Re-reads on a broadcast about the accessibility preferences, and reports a change.
    /// @pre GUI thread.
    /// @param topic What the broadcast concerns.
    void settingsChanged(SettingTopic topic);

  private:
    detail::WindowsSettingReport<config::AccessibilityPreferences> _report;
};

/// Windows' appearance: the accent, the corner radius and material support by build, the scroll-bar style
/// and contrast themes.
class WindowsPlatformAppearance final: public IPlatformAppearance
{
  public:
    /// @param reader The settings reader, shared with the accessibility source.
    /// @param onChange Called on the GUI thread with each changed value; may be empty.
    WindowsPlatformAppearance(std::shared_ptr<IWindowsSettingsReader const> reader,
                              PlatformAppearanceChanged onChange);

    /// @pre GUI thread.
    /// @return The appearance a fresh read means.
    [[nodiscard]] PlatformAppearance current() const override;

    /// Re-reads on a broadcast about the appearance, and reports a change.
    /// @pre GUI thread.
    /// @param topic What the broadcast concerns.
    void settingsChanged(SettingTopic topic);

  private:
    detail::WindowsSettingReport<PlatformAppearance> _report;
};

} // namespace contour::platform
