// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace vtbackend
{

class Terminal;

/// Payload type for an OSC 99 desktop notification sequence.
enum class NotificationPayloadType : uint8_t
{
    Title,
    Body,
    Close,
    Query,
    Alive,
};

/// Urgency level for a desktop notification (maps to freedesktop urgency).
enum class NotificationUrgency : uint8_t
{
    Low = 0,
    Normal = 1,
    Critical = 2,
};

/// When to display the notification based on terminal focus state.
enum class DisplayOccasion : uint8_t
{
    Always,    ///< Always display regardless of focus state.
    Unfocused, ///< Only display when terminal is not focused.
    Invisible, ///< Only display when terminal window is not visible.
};

/// Represents a single OSC 99 desktop notification with all parsed metadata.
struct DesktopNotification
{
    std::string identifier;                                    ///< Notification identifier (`i=`).
    std::string title;                                         ///< Notification title text.
    std::string body;                                          ///< Notification body text.
    std::string applicationName;                               ///< Application name for display (`f=`).
    NotificationUrgency urgency = NotificationUrgency::Normal; ///< Urgency level (`u=`).
    DisplayOccasion occasion = DisplayOccasion::Always;        ///< Display occasion filter (`o=`).
    int timeout = -1;                 ///< Auto-close timeout in ms (`w=`), -1 = server default.
    bool closeEventRequested = false; ///< Report close events back (`c=1`).
    bool focusOnActivation = true;    ///< Focus terminal on click (`a` contains `focus`).
    bool reportOnActivation = false;  ///< Report activation back (`a` contains `report`).
    bool done = true;                 ///< Chunking state: true if complete (`d=1` or absent).
    bool base64Encoded = false;       ///< Payload is base64-encoded (`e=1`).
    NotificationPayloadType currentPayload =
        NotificationPayloadType::Title; ///< Current payload target (`p=`).
};

/// Parses the raw OSC 99 payload (everything after "99;") into a DesktopNotification.
///
/// @param raw the OSC 99 content: "metadata;payload"
/// @return a populated DesktopNotification with parsed metadata and payload.
[[nodiscard]] DesktopNotification parseOSC99(std::string_view raw);

/// Builds a query response string for the `p=?` query.
///
/// @param identifier the notification identifier from the query.
/// @return a formatted OSC 99 response string (without OSC prefix/terminator).
[[nodiscard]] std::string buildOSC99QueryResponse(std::string_view identifier);

/// Manages OSC 99 desktop notification state including chunking and active notification tracking.
class DesktopNotificationManager
{
  public:
    /// Processes a raw OSC 99 sequence and dispatches the appropriate action.
    ///
    /// @param payload the OSC 99 content (everything after the "99;" prefix).
    /// @param terminal the Terminal instance for reply() and focused() access.
    void handleOSC99(std::string_view payload, Terminal& terminal);

    /// Tracks a D-Bus notification ID as active for alive queries.
    void addActiveNotification(std::string const& identifier);

    /// Removes a notification from active tracking (e.g., after close).
    void removeActiveNotification(std::string const& identifier);

    /// Returns the set of currently active notification identifiers.
    [[nodiscard]] std::unordered_set<std::string> const& activeNotifications() const noexcept;

  private:
    /// Pending notifications being assembled via chunking (`d=0`).
    std::unordered_map<std::string, DesktopNotification> _pendingNotifications;

    /// Set of notification identifiers currently displayed.
    std::unordered_set<std::string> _activeNotifications;
};

} // namespace vtbackend
