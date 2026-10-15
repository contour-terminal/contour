// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/NotificationIdMap.hpp>

#include <vtbackend/vt/DesktopNotification.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

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

    /// How long to wait before ASSUMING @p notification was closed, where the desktop cannot say so.
    ///
    /// The assumption exists to answer a close report the application asked for (OSC 99 `c=1`), and
    /// nothing else. A notification that asked for none is never assumed closed: retiring it would
    /// gain nothing and cost everything still to come -- it could no longer be withdrawn, and a
    /// click on it, still sitting in the desktop's notification history, would be dropped as
    /// foreign. It stays mapped until it is replaced, withdrawn or activated instead, or until
    /// NotificationIdMap::Capacity newer notifications push it out.
    ///
    /// @param notification The notification about to be shown.
    /// @param configured The configured fallback. @see resolveCloseDelay().
    /// @return How long to wait, never negative; zero means "never assume it closed".
    [[nodiscard]] static std::chrono::milliseconds assumedCloseDelay(
        vtbackend::DesktopNotification const& notification, std::chrono::milliseconds configured) noexcept
    {
        if (!notification.closeEventRequested)
            return std::chrono::milliseconds { 0 };
        return resolveCloseDelay(notification.timeout, configured);
    }

    /// The server id an outgoing notification should REPLACE, if one with the same OSC identifier is
    /// still live. Freedesktop's Notify takes a replaces_id (0 = new); this resolves it.
    /// @param oscIdentifier The OSC 99 notification identifier about to be sent.
    /// @return The live server id to replace, or 0 for a fresh notification.
    [[nodiscard]] ServerId replacementFor(std::string const& oscIdentifier) const noexcept
    {
        return _ids.serverIdFor(oscIdentifier);
    }

    /// Records that a notification under @p oscIdentifier was handed to the transport, whose reply
    /// has yet to arrive. Every call is answered by exactly one onSent(), successful or not.
    /// @param oscIdentifier The OSC 99 identifier being sent.
    void onSending(std::string const& oscIdentifier) { ++_inFlight[oscIdentifier].sends; }

    /// Records the transport's reply to a send announced through onSending().
    ///
    /// What recording entails -- which stale entries go, and why the caller cannot always name
    /// them -- is stated once, at NotificationIdMap::record(). What this adds is the notification
    /// the CALLER must now withdraw, because a decision about it was taken while it had no id yet:
    /// - this very notification, when a close for its identifier was requested after it was sent;
    /// - the one it superseded, when that was sent under the same identifier but not yet answered
    ///   as this one went out, so this one could not name it as the notification it replaces.
    ///
    /// Replies to one identifier's sends are taken to arrive in the order those sends were made, as
    /// replies on one bus connection do.
    ///
    /// @param oscIdentifier The OSC 99 identifier that was sent.
    /// @param serverId The id the notification server assigned, or nullopt if the send failed.
    /// @param replacedId The server id this notification replaced (0 if it was new).
    /// @return The server id to withdraw now, or nullopt for none.
    [[nodiscard]] std::optional<ServerId> onSent(std::string const& oscIdentifier,
                                                 std::optional<ServerId> serverId,
                                                 ServerId replacedId)
    {
        auto const fate = settleSend(oscIdentifier);
        if (!serverId.has_value())
            return std::nullopt;

        auto const superseded = _ids.serverIdFor(oscIdentifier);
        _ids.record(oscIdentifier, *serverId, replacedId);

        if (fate == SendFate::Withdraw)
            return _ids.takeByIdentifier(oscIdentifier);

        if (superseded != 0 && superseded != *serverId && superseded != replacedId)
            return superseded;

        return std::nullopt;
    }

    /// Resolves the server id to close for an OSC identifier and forgets the mapping. The caller
    /// issues the transport's close only when this returns a value.
    ///
    /// A send under @p oscIdentifier still awaiting its reply has no server id to close yet, so the
    /// close is owed to it instead and onSent() hands it back once the reply names the id. Sends
    /// issued after this call are not affected.
    /// @param oscIdentifier The OSC 99 identifier to close.
    /// @return The server id to close now, or nullopt if the identifier is not live.
    [[nodiscard]] std::optional<ServerId> takeForClose(std::string const& oscIdentifier)
    {
        if (auto const it = _inFlight.find(oscIdentifier); it != _inFlight.end())
            it->second.closesOwed = it->second.sends;

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
    /// What becomes of a send once its reply arrives.
    enum class SendFate : uint8_t
    {
        Keep,     ///< It stays live.
        Withdraw, ///< A close was requested while it was in flight.
    };

    /// The sends under one identifier still awaiting their reply.
    struct InFlight
    {
        uint32_t sends = 0;      ///< How many replies are outstanding.
        uint32_t closesOwed = 0; ///< How many of the oldest of them a close was requested for.
    };

    /// Retires the oldest outstanding send under @p oscIdentifier.
    /// @param oscIdentifier The identifier whose reply arrived.
    /// @return Whether that send is to be withdrawn.
    [[nodiscard]] SendFate settleSend(std::string const& oscIdentifier)
    {
        auto const it = _inFlight.find(oscIdentifier);
        if (it == _inFlight.end())
            return SendFate::Keep;

        auto& inFlight = it->second;
        auto fate = SendFate::Keep;
        if (inFlight.closesOwed > 0)
        {
            --inFlight.closesOwed;
            fate = SendFate::Withdraw;
        }
        if (--inFlight.sends == 0)
            _inFlight.erase(it);
        return fate;
    }

    /// The OSC 99 identifier ⇄ server id bookkeeping. @see NotificationIdMap.
    NotificationIdMap _ids;

    /// The sends whose reply is still to come, by OSC 99 identifier; an identifier with none has no
    /// entry, so this is bounded by the replies outstanding.
    std::unordered_map<std::string, InFlight> _inFlight;
};

} // namespace contour::platform
