// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>

#include <functional>
#include <memory>
#include <optional>

namespace contour::platform
{

/// What the operating system publishes about its own look, for the chrome to follow.
struct PlatformAppearance
{
    std::optional<vtbackend::RGBColor> accent {}; ///< The system accent colour in sRGB, if published.
    std::optional<int> windowCornerRadius {}; ///< The system window corner radius in pixels, if published.

    [[nodiscard]] bool operator==(PlatformAppearance const&) const = default;
};

/// Reads the operating system's appearance.
class IPlatformAppearance
{
  public:
    virtual ~IPlatformAppearance() = default;

    /// @return The appearance as the operating system reports it now.
    /// @pre Called on the GUI thread.
    [[nodiscard]] virtual PlatformAppearance current() const = 0;
};

/// Called on the GUI thread with the new appearance whenever the operating system reports a change.
using PlatformAppearanceChanged = std::function<void(PlatformAppearance)>;

/// The appearance for platforms that publish none: nothing set, never a change.
class NullPlatformAppearance final: public IPlatformAppearance
{
  public:
    /// @return An appearance with no field set.
    [[nodiscard]] PlatformAppearance current() const override { return {}; }
};

/// Creates the native appearance source, or the null source where there is none.
/// @param onChange Called on the GUI thread on every reported change; may be empty.
/// @return The source; it stops reporting changes once destroyed.
[[nodiscard]] std::unique_ptr<IPlatformAppearance> makePlatformAppearance(PlatformAppearanceChanged onChange);

} // namespace contour::platform
