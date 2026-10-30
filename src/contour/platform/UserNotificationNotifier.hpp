// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/Notifier.hpp>
#include <contour/platform/UserNotificationCenter.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace contour::platform
{

/// A Notifier over the application-wide notification centre, for one session or for the application.
///
/// Every request id carries the owner's prefix, so two sessions' OSC 99 identifiers never meet in the
/// centre and a click reaches only the owner it was raised for. Withdraws what it raised when it goes.
/// When the centre turns Unavailable after this was built (the system refused to register the
/// application), everything goes to the fallback instead, so a notice is never silently lost.
/// GUI thread only: the centre's post() and withdraw() are.
class UserNotificationNotifier final: public Notifier
{
    Q_OBJECT

  public:
    /// @param center The centre; it must outlive this notifier.
    /// @param prefix What every request id of this notifier starts with: sessionRequestPrefix() for a
    ///               session, applicationRequestPrefix() for the application's own.
    /// @param closeDelay How long after raising a notification that asked to hear of its close (OSC 99
    ///                   `c=1`) to report it closed, the centre being unable to say; zero never reports.
    /// @param fallback Raises everything while the centre is Unavailable, and its clicks and closes are
    ///                 reported as this notifier's; null raises nothing then. Owned.
    /// @param parent Qt ownership, or nullptr.
    UserNotificationNotifier(IUserNotificationCenter& center,
                             std::string prefix,
                             std::chrono::milliseconds closeDelay,
                             std::unique_ptr<Notifier> fallback,
                             QObject* parent = nullptr);

    /// Withdraws every notification this notifier raised, and unsubscribes from the centre.
    ~UserNotificationNotifier() override;

    UserNotificationNotifier(UserNotificationNotifier const&) = delete;
    UserNotificationNotifier& operator=(UserNotificationNotifier const&) = delete;
    UserNotificationNotifier(UserNotificationNotifier&&) = delete;
    UserNotificationNotifier& operator=(UserNotificationNotifier&&) = delete;

    /// Posts @p notification under this session's prefix. An application's is titled as one, as a tray
    /// message is, so it cannot pass for a notice of Contour's own.
    /// @param notification The notification.
    void notify(vtbackend::DesktopNotification const& notification) override;

    /// Withdraws the notification carrying @p identifier.
    /// @param identifier The OSC 99 identifier it was raised under.
    void close(std::string const& identifier) override;

  private:
    /// Raises @p request through the fallback: the centre held it and then found it cannot raise it.
    void raiseThroughFallback(UserNotificationRequest const& request);

    IUserNotificationCenter& _center;
    std::string _prefix;
    std::chrono::milliseconds _closeDelay;
    std::unique_ptr<Notifier> _fallback;
    UserNotificationSubscription _subscription;
    /// The OSC identifiers raised and not withdrawn since: what the destructor and a later close withdraw,
    /// whether or not an assumed close was already reported for them.
    std::set<std::string> _posted {};
    /// The identifiers with an assumed close still to report, each with the number of its latest posting,
    /// so a timer armed for an earlier posting finds itself superseded.
    std::map<std::string, std::uint64_t> _timed {};
    std::uint64_t _postings = 0;
};

} // namespace contour::platform
