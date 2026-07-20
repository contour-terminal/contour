// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/DesktopNotification.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace contour
{

/// The dependency-free identifier-mapping and urgency policy shared by the desktop-notification
/// backends. It owns the bidirectional OSC-id ⇄ server-id bookkeeping the D-Bus transport would
/// otherwise tangle with the wire calls, so the routing logic (replace-in-place, close cleanup,
/// server-close/activation dispatch) is unit-testable without a D-Bus session.
///
/// The transport-specific numeric id type is @p ServerId (a D-Bus notification id is a uint32).
class NotificationRouter
{
  public:
    using ServerId = uint32_t;

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

    /// The server id an outgoing notification should REPLACE, if one with the same OSC identifier is
    /// still live. Freedesktop's Notify takes a replaces_id (0 = new); this resolves it.
    /// @param oscIdentifier The OSC 99 notification identifier about to be sent.
    /// @return The live server id to replace, or 0 for a fresh notification.
    [[nodiscard]] ServerId replacementFor(std::string const& oscIdentifier) const noexcept
    {
        auto const it = _oscToServer.find(oscIdentifier);
        return it != _oscToServer.end() ? it->second : ServerId { 0 };
    }

    /// Records the server id the transport returned for a sent notification, updating the
    /// bidirectional mapping and dropping the stale reverse entry when this replaced @p replacedId.
    /// @param oscIdentifier The OSC 99 identifier that was sent.
    /// @param serverId The id the notification server assigned.
    /// @param replacedId The server id this notification replaced (0 if it was new).
    void onSent(std::string const& oscIdentifier, ServerId serverId, ServerId replacedId)
    {
        if (replacedId != 0)
            _serverToOsc.erase(replacedId);
        _serverToOsc[serverId] = oscIdentifier;
        _oscToServer[oscIdentifier] = serverId;
    }

    /// Resolves the server id to close for an OSC identifier and forgets the mapping. The caller
    /// issues the transport's close only when this returns a value.
    /// @param oscIdentifier The OSC 99 identifier to close.
    /// @return The server id to close, or nullopt if the identifier is not live.
    [[nodiscard]] std::optional<ServerId> takeForClose(std::string const& oscIdentifier)
    {
        auto const it = _oscToServer.find(oscIdentifier);
        if (it == _oscToServer.end())
            return std::nullopt;
        auto const serverId = it->second;
        _serverToOsc.erase(serverId);
        _oscToServer.erase(it);
        return serverId;
    }

    /// Resolves a server-side close/activation event back to its OSC identifier and forgets the
    /// mapping (both event kinds retire the notification). The caller emits its signal only when
    /// this returns a value.
    /// @param serverId The server id reported by the NotificationClosed / ActionInvoked signal.
    /// @return The OSC identifier, or nullopt if the server id is unknown (a foreign notification).
    [[nodiscard]] std::optional<std::string> takeForServerEvent(ServerId serverId)
    {
        auto const it = _serverToOsc.find(serverId);
        if (it == _serverToOsc.end())
            return std::nullopt;
        auto oscIdentifier = it->second;
        _oscToServer.erase(oscIdentifier);
        _serverToOsc.erase(it);
        return oscIdentifier;
    }

  private:
    std::unordered_map<ServerId, std::string> _serverToOsc;
    std::unordered_map<std::string, ServerId> _oscToServer;
};

} // namespace contour
