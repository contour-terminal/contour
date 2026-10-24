// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>

#include <functional>
#include <memory>

namespace contour::platform
{

/// Reads the operating system's accessibility display preferences.
///
/// The source is the adapter; what the chrome does with the preferences is decided by the token
/// resolver, which consumes the plain `config::AccessibilityPreferences` value.
class IAccessibilityPreferenceSource
{
  public:
    virtual ~IAccessibilityPreferenceSource() = default;

    /// @return The preferences as the operating system reports them now.
    /// @pre Called on the GUI thread.
    [[nodiscard]] virtual config::AccessibilityPreferences current() const = 0;
};

/// Called on the GUI thread with the new preferences whenever the operating system reports a change.
using AccessibilityPreferencesChanged = std::function<void(config::AccessibilityPreferences)>;

/// The source for platforms that publish no accessibility preferences: always the defaults, never a
/// change.
class NullAccessibilityPreferenceSource final: public IAccessibilityPreferenceSource
{
  public:
    /// @return The default preferences.
    [[nodiscard]] config::AccessibilityPreferences current() const override { return {}; }
};

/// Creates the native accessibility preference source, or the null source where there is none.
/// @param onChange Called on the GUI thread on every reported change; may be empty.
/// @return The source; it stops reporting changes once destroyed.
[[nodiscard]] std::unique_ptr<IAccessibilityPreferenceSource> makeAccessibilityPreferenceSource(
    AccessibilityPreferencesChanged onChange);

} // namespace contour::platform
