// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/LRUCache.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

/// How the terminal came to know a notification was closed.
///
/// Not every desktop can say. org.freedesktop.Notifications emits a close signal, but
/// org.freedesktop.portal.Notification -- the only service a Flatpak sandbox can reach -- has no
/// equivalent, so there the terminal can at best assume the popup was retired once enough time has
/// passed. OSC 99 has a word for exactly that case, and this is what selects it.
enum class CloseReport : uint8_t
{
    Observed = 0,  ///< The desktop reported the close.
    Untracked = 1, ///< Nobody reported it; a timer expired.
};

/// Names a CloseReport, for diagnostics.
///
/// A switch rather than a conditional at each use site: a third way of coming to know about a close
/// would then be a compile error here instead of a log line that quietly says the wrong word.
/// @param report the provenance to name.
/// @return the label.
[[nodiscard]] constexpr std::string_view closeReportName(CloseReport report) noexcept
{
    switch (report)
    {
        case CloseReport::Observed: return "observed";
        case CloseReport::Untracked: return "untracked";
    }
    return "observed";
}

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

/// Builds the close report for a notification the desktop retired.
///
/// A close the terminal could not observe is marked `untracked`, which is what the protocol defines
/// for a platform whose desktop does not report closure. Reporting it as a bare close instead would
/// tell the application the popup is gone as though we had watched it happen.
///
/// @param identifier the notification identifier being reported on.
/// @param report how the terminal came to know about the close.
/// @return a formatted OSC 99 response string (without OSC prefix/terminator).
[[nodiscard]] std::string buildOSC99CloseResponse(std::string_view identifier, CloseReport report);

/// How much OSC 99 state a terminal keeps. Past either list bound, the entry used longest ago is
/// dropped; past the text bound, the rest of a title or body is.
///
/// An application chooses every identifier, so one that keeps inventing new ones -- chunks it never
/// finishes, notifications whose close is never observed -- would otherwise grow either list for the
/// life of the session. Sending a chunk or a notification again under its identifier counts as a use,
/// so an identifier an application keeps reusing outlives a burst of others. And it chooses how many
/// chunks a notification has, so without the text bound one unfinished notification grows as long.
///
/// kitty bounds its live notifications at 128 and evicts the oldest (`kitty/notifications.py:1026-1033`,
/// `register_in_progress_notification`); its C side only forwards the sequence. Of unfinished ones it
/// keeps a single notification per window (`pending_commands`, keyed by window in
/// `handle_notification_cmd`, `kitty/notifications.py:1089-1097`), so a second identifier's chunks
/// displace the first's. Contour assembles by identifier instead, so interleaved notifications both
/// arrive, and bounds them at the same 128. kitty keeps the first 1024 code points of a title and of a
/// body and silently drops the rest (`limit_size`, `kitty/notifications.py:221-224`, applied as each
/// chunk is committed at 409 and 411). Contour counts bytes instead, and a code point is at most four of
/// them, so its 4096 keep at least what kitty keeps.
struct DesktopNotificationLimits
{
    size_t pending = 128;    ///< Chunked notifications (`d=0`) still waiting for their last chunk.
    size_t live = 128;       ///< Identifiers `p=alive` reports.
    size_t textBytes = 4096; ///< UTF-8 bytes a title, and a body, keeps; cut on a code point boundary.
};

/// The limits every terminal keeps. The GUI's identifier bookkeeping keeps the same live bound.
inline constexpr auto NotificationLimits = DesktopNotificationLimits {};

/// Manages OSC 99 desktop notification state including chunking and active notification tracking.
class DesktopNotificationManager
{
  public:
    /// Processes a raw OSC 99 sequence and dispatches the appropriate action.
    ///
    /// @param payload the OSC 99 content (everything after the "99;" prefix).
    /// @param terminal the Terminal instance it replies through and dispatches to.
    void handleOSC99(std::string_view payload, Terminal& terminal);

    /// Tracks a notification identifier as active for alive queries, as the newest one.
    /// @param identifier The identifier the notification was raised under.
    void addActiveNotification(std::string const& identifier);

    /// Removes a notification from active tracking (e.g., after close).
    void removeActiveNotification(std::string const& identifier);

    /// @return The identifiers currently tracked as active, newest first.
    [[nodiscard]] std::vector<std::string> activeNotifications() const;

    /// @return The identifiers of the chunked notifications still being assembled, newest first.
    [[nodiscard]] std::vector<std::string> pendingNotifications() const;

  private:
    /// What the active list holds per identifier: nothing, for the identifier is all `p=alive` reports.
    struct Live
    {
    };

    /// Pending notifications being assembled via chunking (`d=0`), bounded by NotificationLimits.
    crispy::LRUCache<std::string, DesktopNotification> _pendingNotifications { NotificationLimits.pending };

    /// The identifiers of the notifications currently displayed, bounded by NotificationLimits.
    crispy::LRUCache<std::string, Live> _activeNotifications { NotificationLimits.live };
};

} // namespace vtbackend
