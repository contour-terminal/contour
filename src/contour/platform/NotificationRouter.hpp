// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NotificationIdMap.hpp>

#include <vtbackend/DesktopNotification.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace contour::platform
{

/// The dependency-free routing policy the desktop-notification backends share: it says what the
/// OSC 99 identifiers MEAN -- which notification an outgoing one replaces, which one a close or an
/// activation is about, and how long to wait before assuming a close nobody reported. The
/// bookkeeping underneath is NotificationIdMap; this names it in the protocol's own vocabulary, so
/// the routing logic is unit-testable without a D-Bus session.
///
/// It speaks NEITHER wire format. How an urgency is spelled is the sending transport's business and
/// lives beside the call that carries it -- @see buildFreedesktopNotifyArguments() and
/// buildPortalNotificationOptions() -- so a third backend adds its table to its own header rather
/// than to this one.
///
/// The transport-specific numeric id type is @p ServerId (a D-Bus notification id is a uint32).
class NotificationRouter
{
  public:
    using ServerId = NotificationIdMap::ServerId;

    /// How long to wait before ASSUMING a notification was closed, where the desktop cannot say so.
    ///
    /// A notification states its own lifetime through OSC 99's `w=`, and that always wins: the
    /// application knows what it asked for. Only `w=-1` -- "use whatever the desktop does" -- has
    /// nothing to go on, and falls back to the configured guess.
    ///
    /// @param timeout The notification's `w=` in milliseconds: >0 an explicit lifetime, 0 "never
    ///                auto-close" (kitty's meaning), <0 "the desktop's default".
    /// @param configured The configured fallback; zero or less disables the assumption entirely.
    /// @return How long to wait, never negative; zero means "never assume it closed".
    [[nodiscard]] static constexpr std::chrono::milliseconds resolveCloseDelay(
        int timeout, std::chrono::milliseconds configured) noexcept
    {
        if (timeout > 0)
            return std::chrono::milliseconds { timeout };
        if (timeout == 0)
            return std::chrono::milliseconds { 0 };

        // A negative configuration value reads as "disabled" rather than travelling on to become a
        // negative timer interval, which Qt refuses to start and complains about instead.
        return std::max(configured, std::chrono::milliseconds { 0 });
    }

    /// The server id an outgoing notification should REPLACE, if one with the same OSC identifier is
    /// still live. Freedesktop's Notify takes a replaces_id (0 = new); this resolves it.
    /// @param oscIdentifier The OSC 99 notification identifier about to be sent.
    /// @return The live server id to replace, or 0 for a fresh notification.
    [[nodiscard]] ServerId replacementFor(std::string const& oscIdentifier) const noexcept
    {
        return _ids.serverIdFor(oscIdentifier);
    }

    /// Records the server id the transport returned for a sent notification.
    ///
    /// What that entails -- which stale entries go, and why the caller cannot always name them --
    /// is stated once, at NotificationIdMap::record().
    /// @param oscIdentifier The OSC 99 identifier that was sent.
    /// @param serverId The id the notification server assigned.
    /// @param replacedId The server id this notification replaced (0 if it was new).
    void onSent(std::string const& oscIdentifier, ServerId serverId, ServerId replacedId)
    {
        _ids.record(oscIdentifier, serverId, replacedId);
    }

    /// Resolves the server id to close for an OSC identifier and forgets the mapping. The caller
    /// issues the transport's close only when this returns a value.
    /// @param oscIdentifier The OSC 99 identifier to close.
    /// @return The server id to close, or nullopt if the identifier is not live.
    [[nodiscard]] std::optional<ServerId> takeForClose(std::string const& oscIdentifier)
    {
        return _ids.takeByIdentifier(oscIdentifier);
    }

    /// Resolves a server-side close/activation event back to its OSC identifier and forgets the
    /// mapping (both event kinds retire the notification). The caller emits its signal only when
    /// this returns a value.
    /// @param serverId The server id reported by the NotificationClosed / ActionInvoked signal.
    /// @return The OSC identifier, or nullopt if the server id is unknown (a foreign notification).
    [[nodiscard]] std::optional<std::string> takeForServerEvent(ServerId serverId)
    {
        return _ids.takeByServerId(serverId);
    }

  private:
    /// The OSC 99 identifier ⇄ server id bookkeeping. @see NotificationIdMap.
    NotificationIdMap _ids;
};

} // namespace contour::platform
