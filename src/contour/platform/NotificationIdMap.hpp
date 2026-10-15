// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/DesktopNotification.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace contour::platform
{

/// The identifier ⇄ server-id bookkeeping a desktop-notification backend needs, and the one place
/// its invariant lives.
///
/// The relation has to be answerable from both ends: an outgoing notification asks "is one already
/// live under this identifier, to replace?", while an incoming close or activation arrives naming
/// only the numeric id. So it is held twice, in opposite directions, as exact mutual inverses --
/// every entry in one direction has its counterpart in the other, and retiring a notification drops
/// both. A third index, in the order the notifications were recorded, is what keeps it bounded.
/// @see Capacity
///
/// It is a type of its own because it was written twice: NotificationRouter maps the OSC 99
/// identifier to the id the notification server assigned, PortalNotificationTransport maps the
/// portal's application-chosen id to the one it mints itself. Two copies of one invariant is two
/// places to break it -- as both were, in the same way, by the same missing retirement.
class NotificationIdMap
{
  public:
    /// A notification's numeric id. Zero is freedesktop's "this is a new notification" sentinel, so
    /// it never names a live notification and is safe as the "nothing is live" answer.
    using ServerId = uint32_t;

    /// How many notifications are kept live at once; past it, the one sent longest ago is forgotten.
    ///
    /// A desktop that cannot report a close -- the notification portal -- leaves a notification mapped
    /// until it is replaced, withdrawn or clicked, so an application sending ever new identifiers would
    /// otherwise grow this without bound. A forgotten notification stays on the desktop, but can no
    /// longer be withdrawn, and a click on it is dropped as foreign. Re-sending an identifier makes it the
    /// newest again, so a notification that keeps being replaced -- a session's finish notice -- outlives
    /// a burst of others. kitty bounds its own the same way, at the same size (`notifications.py`,
    /// `register_in_progress_notification`). It is the terminal's own live bound, so the identifiers
    /// `p=alive` reports and the notifications kept here are bounded alike.
    static constexpr size_t Capacity = vtbackend::NotificationLimits.live;

    /// The id live under @p identifier, if any.
    /// @param identifier The identifier to look up.
    /// @return The live id, or 0 when nothing is live under it.
    [[nodiscard]] ServerId serverIdFor(std::string const& identifier) const noexcept
    {
        auto const it = _byIdentifier.find(identifier);
        return it != _byIdentifier.end() ? it->second : ServerId { 0 };
    }

    /// Records @p serverId as the notification live under @p identifier, retiring whatever was live
    /// under either name before.
    ///
    /// @p replacedId is retired as well as -- not instead of -- whatever @p identifier already
    /// resolved to, because the caller cannot always name it: a notification sent under an
    /// identifier whose previous send has not been answered yet has no id to name. Left behind,
    /// that earlier entry answers its own close by retiring the LIVE notification, which can then
    /// be neither closed nor replaced.
    ///
    /// @param identifier The identifier the notification was sent under.
    /// @param serverId The id now live under it.
    /// @param replacedId An id this notification supersedes, or 0 for none.
    void record(std::string const& identifier, ServerId serverId, ServerId replacedId)
    {
        forget(replacedId);
        forget(serverIdFor(identifier));

        // A notification server is free to reissue an id once the notification wearing it is gone,
        // so the incoming id is cleared of any stale identifier too.
        forget(serverId);

        auto const sent = ++_lastSent;
        _byServerId[serverId] = Entry { .identifier = identifier, .sent = sent };
        _byIdentifier[identifier] = serverId;
        _bySent.emplace(sent, serverId);

        if (_bySent.size() > Capacity)
            forget(_bySent.begin()->second);
    }

    /// Resolves @p identifier to its live id and retires it.
    /// @param identifier The identifier to resolve.
    /// @return The id, or nullopt when nothing is live under it.
    [[nodiscard]] std::optional<ServerId> takeByIdentifier(std::string const& identifier)
    {
        auto const serverId = serverIdFor(identifier);
        if (!forget(serverId).has_value())
            return std::nullopt;
        return serverId;
    }

    /// Resolves @p serverId to its identifier and retires it.
    /// @param serverId The id to resolve.
    /// @return The identifier, or nullopt when the id names nothing of ours.
    [[nodiscard]] std::optional<std::string> takeByServerId(ServerId serverId) { return forget(serverId); }

  private:
    /// A live notification, as its server id finds it.
    struct Entry
    {
        std::string identifier; ///< The identifier it was sent under.
        uint64_t sent = 0;      ///< Its place in the order of records: its key in _bySent.
    };

    /// Retires @p serverId in every direction; a no-op where it names nothing live. The one place an
    /// entry leaves, so the three maps cannot drift apart.
    /// @param serverId The id to forget.
    /// @return The identifier it was live under, or nullopt when it named nothing live.
    std::optional<std::string> forget(ServerId serverId)
    {
        auto const it = _byServerId.find(serverId);
        if (it == _byServerId.end())
            return std::nullopt;

        auto entry = std::move(it->second);
        _byServerId.erase(it);
        _byIdentifier.erase(entry.identifier);
        _bySent.erase(entry.sent);
        return std::move(entry.identifier);
    }

    std::unordered_map<ServerId, Entry> _byServerId;
    std::unordered_map<std::string, ServerId> _byIdentifier;

    /// The live server ids, the one recorded longest ago first: where Capacity evicts from.
    std::map<uint64_t, ServerId> _bySent;

    /// The last record's place in _bySent.
    uint64_t _lastSent = 0;
};

} // namespace contour::platform
