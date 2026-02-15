// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/DesktopNotification.h>
#include <vtbackend/Terminal.h>

#include <crispy/base64.h>
#include <crispy/utils.h>

#include <charconv>
#include <format>
#include <string>
#include <string_view>

using std::string;
using std::string_view;

namespace vtbackend
{

namespace
{
    /// Parses the colon-separated key=value metadata portion of an OSC 99 sequence.
    void parseMetadata(string_view metadata, DesktopNotification& notification)
    {
        crispy::for_each_key_value(
            crispy::for_each_key_value_params {
                .text = metadata,
                .entryDelimiter = ':',
                .assignmentDelimiter = '=',
            },
            [&](string_view key, string_view value) {
                if (key == "i")
                    notification.identifier = string(value);
                else if (key == "p")
                {
                    if (value == "title")
                        notification.currentPayload = NotificationPayloadType::Title;
                    else if (value == "body")
                        notification.currentPayload = NotificationPayloadType::Body;
                    else if (value == "close")
                        notification.currentPayload = NotificationPayloadType::Close;
                    else if (value == "?")
                        notification.currentPayload = NotificationPayloadType::Query;
                    else if (value == "alive")
                        notification.currentPayload = NotificationPayloadType::Alive;
                }
                else if (key == "e")
                    notification.base64Encoded = (value == "1");
                else if (key == "d")
                    notification.done = (value != "0");
                else if (key == "u")
                {
                    if (value == "0")
                        notification.urgency = NotificationUrgency::Low;
                    else if (value == "1")
                        notification.urgency = NotificationUrgency::Normal;
                    else if (value == "2")
                        notification.urgency = NotificationUrgency::Critical;
                }
                else if (key == "o")
                {
                    if (value == "always")
                        notification.occasion = DisplayOccasion::Always;
                    else if (value == "unfocused")
                        notification.occasion = DisplayOccasion::Unfocused;
                    else if (value == "invisible")
                        notification.occasion = DisplayOccasion::Invisible;
                }
                else if (key == "f")
                    notification.applicationName = string(value);
                else if (key == "w")
                {
                    auto const str = string(value);
                    int timeout = -1;
                    auto const [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), timeout);
                    if (ec == std::errc())
                        notification.timeout = timeout;
                }
                else if (key == "c")
                    notification.closeEventRequested = (value == "1");
                else if (key == "a")
                {
                    // a= can be a comma-separated list: "focus", "report", "focus,report"
                    notification.focusOnActivation = false;
                    notification.reportOnActivation = false;
                    crispy::split(value, ',', [&](string_view part) {
                        if (part == "focus")
                            notification.focusOnActivation = true;
                        else if (part == "report")
                            notification.reportOnActivation = true;
                        return true;
                    });
                }
                // Unknown keys are silently ignored per Kitty protocol spec.
            });
    }

    /// Applies payload text to the notification based on the current payload type.
    void applyPayload(string_view payloadText, DesktopNotification& notification)
    {
        auto const decoded =
            notification.base64Encoded ? crispy::base64::decode(payloadText) : string(payloadText);

        switch (notification.currentPayload)
        {
            case NotificationPayloadType::Title: notification.title += decoded; break;
            case NotificationPayloadType::Body: notification.body += decoded; break;
            case NotificationPayloadType::Close:
            case NotificationPayloadType::Query:
            case NotificationPayloadType::Alive:
                // These payload types don't carry text data.
                break;
        }
    }

    /// Checks whether the notification should be displayed given the terminal's focus state.
    [[nodiscard]] bool shouldDisplay(DesktopNotification const& notification, bool terminalFocused)
    {
        switch (notification.occasion)
        {
            case DisplayOccasion::Always: return true;
            case DisplayOccasion::Unfocused: return !terminalFocused;
            case DisplayOccasion::Invisible:
                // We treat "invisible" the same as "unfocused" since we don't track
                // window visibility state separately from focus.
                return !terminalFocused;
        }
        return true;
    }
} // namespace

DesktopNotification parseOSC99(string_view raw)
{
    auto notification = DesktopNotification {};

    // Split at the first ';' to separate metadata from payload.
    auto const semicolonPos = raw.find(';');
    if (semicolonPos == string_view::npos)
    {
        // No semicolon: treat entire input as metadata with empty payload.
        parseMetadata(raw, notification);
        return notification;
    }

    auto const metadata = raw.substr(0, semicolonPos);
    auto const payload = raw.substr(semicolonPos + 1);

    parseMetadata(metadata, notification);
    applyPayload(payload, notification);

    return notification;
}

string buildOSC99QueryResponse(string_view identifier)
{
    return std::format("99;i={}:p=?;"
                       "a=focus,report:"
                       "o=always,unfocused,invisible:"
                       "u=0,1,2:"
                       "p=title,body,?,close,alive:"
                       "c=1:"
                       "w=1",
                       identifier);
}

void DesktopNotificationManager::handleOSC99(string_view payload, Terminal& terminal)
{
    auto notification = parseOSC99(payload);

    switch (notification.currentPayload)
    {
        case NotificationPayloadType::Query: {
            // Respond with supported capabilities.
            auto const response = buildOSC99QueryResponse(notification.identifier);
            terminal.reply("\033]{}\033\\", response);
            return;
        }
        case NotificationPayloadType::Alive: {
            // Respond with comma-separated list of active notification IDs.
            string idList;
            for (auto const& id: _activeNotifications)
            {
                if (!idList.empty())
                    idList += ',';
                idList += id;
            }
            terminal.reply("\033]99;i={}:p=alive;{}\033\\", notification.identifier, idList);
            return;
        }
        case NotificationPayloadType::Close: {
            // Request to close a notification.
            terminal.discardDesktopNotification(notification.identifier);
            removeActiveNotification(notification.identifier);
            return;
        }
        case NotificationPayloadType::Title:
        case NotificationPayloadType::Body: {
            // Handle chunking: if d=0 (not done), store for later assembly.
            if (!notification.done)
            {
                auto it = _pendingNotifications.find(notification.identifier);
                if (it == _pendingNotifications.end())
                {
                    // First chunk — store the notification.
                    _pendingNotifications.emplace(notification.identifier, std::move(notification));
                }
                else
                {
                    // Subsequent chunk — merge payload into existing notification.
                    auto& pending = it->second;
                    if (notification.currentPayload == NotificationPayloadType::Title)
                        pending.title += notification.title;
                    else if (notification.currentPayload == NotificationPayloadType::Body)
                        pending.body += notification.body;
                    // Update other metadata fields from the latest chunk.
                    if (!notification.applicationName.empty())
                        pending.applicationName = std::move(notification.applicationName);
                    pending.urgency = notification.urgency;
                    pending.occasion = notification.occasion;
                    pending.closeEventRequested =
                        pending.closeEventRequested || notification.closeEventRequested;
                    pending.focusOnActivation = notification.focusOnActivation;
                    pending.reportOnActivation = notification.reportOnActivation;
                    if (notification.timeout >= 0)
                        pending.timeout = notification.timeout;
                }
                return;
            }

            // d=1 (or default): finalize. Check if there's a pending notification to merge.
            if (auto it = _pendingNotifications.find(notification.identifier);
                it != _pendingNotifications.end())
            {
                auto& pending = it->second;
                // Merge final chunk payload.
                if (notification.currentPayload == NotificationPayloadType::Title)
                    pending.title += notification.title;
                else if (notification.currentPayload == NotificationPayloadType::Body)
                    pending.body += notification.body;
                // Take the assembled notification.
                notification = std::move(pending);
                notification.done = true;
                _pendingNotifications.erase(it);
            }

            // Check occasion filter.
            if (!shouldDisplay(notification, terminal.focused()))
                return;

            // Dispatch to the terminal event listener.
            addActiveNotification(notification.identifier);
            terminal.showDesktopNotification(notification);
            return;
        }
    }
}

void DesktopNotificationManager::addActiveNotification(string const& identifier)
{
    _activeNotifications.insert(identifier);
}

void DesktopNotificationManager::removeActiveNotification(string const& identifier)
{
    _activeNotifications.erase(identifier);
}

std::unordered_set<string> const& DesktopNotificationManager::activeNotifications() const noexcept
{
    return _activeNotifications;
}

} // namespace vtbackend
