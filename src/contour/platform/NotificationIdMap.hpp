// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
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
/// both.
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

        _byServerId[serverId] = identifier;
        _byIdentifier[identifier] = serverId;
    }

    /// Resolves @p identifier to its live id and retires it.
    /// @param identifier The identifier to resolve.
    /// @return The id, or nullopt when nothing is live under it.
    [[nodiscard]] std::optional<ServerId> takeByIdentifier(std::string const& identifier)
    {
        auto const it = _byIdentifier.find(identifier);
        if (it == _byIdentifier.end())
            return std::nullopt;

        auto const serverId = it->second;
        _byServerId.erase(serverId);
        _byIdentifier.erase(it);
        return serverId;
    }

    /// Resolves @p serverId to its identifier and retires it.
    /// @param serverId The id to resolve.
    /// @return The identifier, or nullopt when the id names nothing of ours.
    [[nodiscard]] std::optional<std::string> takeByServerId(ServerId serverId)
    {
        auto const it = _byServerId.find(serverId);
        if (it == _byServerId.end())
            return std::nullopt;

        auto identifier = it->second;
        _byIdentifier.erase(identifier);
        _byServerId.erase(it);
        return identifier;
    }

  private:
    /// Retires @p serverId in both directions; a no-op where it names nothing live.
    /// @param serverId The id to forget.
    void forget(ServerId serverId)
    {
        auto const it = _byServerId.find(serverId);
        if (it == _byServerId.end())
            return;

        _byIdentifier.erase(it->second);
        _byServerId.erase(it);
    }

    std::unordered_map<ServerId, std::string> _byServerId;
    std::unordered_map<std::string, ServerId> _byIdentifier;
};

} // namespace contour::platform
