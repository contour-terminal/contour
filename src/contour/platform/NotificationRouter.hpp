// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NotificationIdMap.hpp>

#include <vtbackend/DesktopNotification.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace contour::platform
{

/// The dependency-free routing policy the desktop-notification backends share: it says what the
/// OSC 99 identifiers MEAN -- which notification an outgoing one replaces, which one a close or an
/// activation is about, and how long to wait before assuming a close nobody reported. The
/// bookkeeping underneath is NotificationIdMap; this names it in the protocol's own vocabulary, so
/// the routing logic is unit-testable without a D-Bus session.
///
/// The transport-specific numeric id type is @p ServerId (a D-Bus notification id is a uint32).
class NotificationRouter
{
  public:
    using ServerId = NotificationIdMap::ServerId;

    /// Maps a notification urgency onto the freedesktop.org urgency byte (0=low, 1=normal, 2=critical).
    /// @param urgency The backend urgency level.
    /// @return The freedesktop urgency byte; Normal (1) for any unrecognized value.
    [[nodiscard]] static constexpr uint8_t toFreedesktopUrgency(
        vtbackend::NotificationUrgency urgency) noexcept
    {
        switch (urgency)
        {
            case vtbackend::NotificationUrgency::Low: return 0;
            case vtbackend::NotificationUrgency::Normal: return 1;
            case vtbackend::NotificationUrgency::Critical: return 2;
        }
        return 1;
    }

    /// Maps a notification urgency onto the xdg-desktop-portal `priority` string.
    ///
    /// The portal names four levels where freedesktop has three, so its `high` has no counterpart
    /// here and stays unused; Critical maps to `urgent`, the level that survives Do-Not-Disturb.
    /// @param urgency The backend urgency level.
    /// @return The portal priority string; `normal` for any unrecognized value.
    [[nodiscard]] static constexpr std::string_view toPortalPriority(
        vtbackend::NotificationUrgency urgency) noexcept
    {
        switch (urgency)
        {
            case vtbackend::NotificationUrgency::Low: return "low";
            case vtbackend::NotificationUrgency::Normal: return "normal";
            case vtbackend::NotificationUrgency::Critical: return "urgent";
        }
        return "normal";
    }

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

    /// Records the server id the transport returned for a sent notification, updating the
    /// bidirectional mapping and dropping the stale reverse entry when this replaced @p replacedId.
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
