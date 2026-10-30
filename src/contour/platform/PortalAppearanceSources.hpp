// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The appearance and accessibility sources backed by the XDG Settings portal. Qt-free and compiled on
/// every platform: they speak to the portal only through an injected IPortalSettingsChannel, so they
/// are tested over a fake one, and only the Linux factory hands them the D-Bus channel.

#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/platform/PortalSettings.hpp>
#include <contour/platform/PortalSettingsChannel.hpp>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace contour::platform
{

/// The namespaces the appearance source asks for: the accent and light/dark, and the button layout.
inline constexpr std::array PortalAppearanceNamespaces { AppearanceNamespace, GnomeWmNamespace };
/// The namespaces the accessibility source asks for.
inline constexpr std::array PortalAccessibilityNamespaces { AppearanceNamespace };

/// The portal's latest reading for a set of namespaces: the reply to one readAll, with every later
/// change merged in. Reports each update to its owner, which decides whether it changed anything.
class PortalReadingCache
{
  public:
    /// Called on the GUI thread after the reading was updated.
    /// @param before The reading as it was until this update.
    using Updated = std::function<void(PortalReading const& before)>;

    /// Builds the channel and asks it for everything, without waiting for the answer.
    ///
    /// A reply that arrives before the constructor returns (the null channel's) is the starting
    /// reading, not an update: @p onUpdate is not called for it, as the owner reads the reading once
    /// it exists.
    /// @param channels Builds the channel.
    /// @param namespaces The namespaces to ask for.
    /// @param onUpdate Called after every later update.
    PortalReadingCache(PortalSettingsChannelFactory const& channels,
                       std::span<std::string_view const> namespaces,
                       Updated onUpdate);

    // The channel's handlers capture this: a copy or a move would leave them pointing at the old object.
    PortalReadingCache(PortalReadingCache const&) = delete;
    PortalReadingCache& operator=(PortalReadingCache const&) = delete;
    PortalReadingCache(PortalReadingCache&&) = delete;
    PortalReadingCache& operator=(PortalReadingCache&&) = delete;
    ~PortalReadingCache() = default;

    /// @return The reading so far: no settings before a reply, then the portal's settings or why there
    ///         are none.
    [[nodiscard]] PortalReading const& reading() const noexcept { return _reading; }

  private:
    void update(PortalReading next);

    PortalReading _reading = PortalSettings {};
    /// Held back while the constructor's own readAll runs; see the constructor.
    Updated _onUpdate;
    /// Last, so it dies first: nothing it holds can then reach the members above.
    std::unique_ptr<IPortalSettingsChannel> _channel;
};

/// The platform appearance as the Settings portal publishes it (§2.4).
class PortalPlatformAppearance final: public IPlatformAppearance
{
  public:
    /// Builds the channel for PortalAppearanceNamespaces and asks it for everything.
    /// @param channels Builds the channel.
    /// @param desktop What the button layout is resolved from besides the portal: the desktop's files,
    ///                read once by the caller, and the session.
    /// @param onChange Called on the GUI thread, once per reply or change that alters current(); may be
    ///                 empty.
    PortalPlatformAppearance(PortalSettingsChannelFactory const& channels,
                             DesktopLayoutInputs desktop,
                             PlatformAppearanceChanged onChange);

    /// @return appearanceFromPortal() of the reading so far: before any reply, no accent and the files'
    ///         button layout.
    [[nodiscard]] PlatformAppearance current() const override;

  private:
    DesktopLayoutInputs _desktop;
    PlatformAppearanceChanged _onChange;
    /// Last, so the channel dies first.
    PortalReadingCache _cache;
};

/// The accessibility preferences as the Settings portal publishes them (§2.3).
class PortalAccessibilityPreferenceSource final: public IAccessibilityPreferenceSource
{
  public:
    /// Builds the channel for PortalAccessibilityNamespaces and asks it for everything.
    /// @param channels Builds the channel.
    /// @param onChange Called on the GUI thread, once per reply or change that alters current(); may be
    ///                 empty.
    PortalAccessibilityPreferenceSource(PortalSettingsChannelFactory const& channels,
                                        AccessibilityPreferencesChanged onChange);

    /// @return accessibilityFromPortal() of the settings so far: the defaults without any.
    [[nodiscard]] config::AccessibilityPreferences current() const override;

  private:
    AccessibilityPreferencesChanged _onChange;
    /// Last, so the channel dies first.
    PortalReadingCache _cache;
};

} // namespace contour::platform
