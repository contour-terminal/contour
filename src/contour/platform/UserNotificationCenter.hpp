// SPDX-License-Identifier: Apache-2.0
//
// The operating system's user-notification centre as a seam: one per application, shared by every
// session's notifier. Qt-free and header-only, so the routing a click takes and the authorization gate
// are table-tested in contour_test; the native adapter (Notification Center on macOS) lives beside it.
#pragma once

#include <contour/platform/AppBundle.hpp>

#include <vtbackend/vt/DesktopNotification.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::platform
{

/// One notification handed to the centre.
struct UserNotificationRequest
{
    std::string requestId {};   ///< Unique across the application: a session's prefix, then the OSC id.
    std::string title {};       ///< The notification's title, as the centre shows it.
    std::string sourceTitle {}; ///< The title as the application sent it, which a fallback is handed.
    std::string body {};        ///< The notification's body.
    vtbackend::NotificationSound sound = vtbackend::NotificationSound::DesktopDefault; ///< Its sound.
    std::string threadId {}; ///< What the centre groups it with: the session's prefix.

    /// Member-wise equality.
    [[nodiscard]] bool operator==(UserNotificationRequest const&) const = default;
};

/// What a click on a notification reports: the OSC 99 identifier after the subscriber's prefix.
using UserNotificationActivated = std::function<void(std::string_view oscIdentifier)>;

/// What a request the centre could not raise reports, so its owner can raise it another way.
using UserNotificationUndelivered = std::function<void(UserNotificationRequest const& request)>;

/// Whether the centre can raise anything at all.
enum class UserNotificationAvailability : uint8_t
{
    Unavailable = 0, ///< Nothing is raised; callers keep their own path (the tray).
    Available,       ///< Requests are raised on the operating system's centre.
};

/// A subscriber's claim on the clicks that carry its prefix. Move-only; releasing it unsubscribes.
///
/// Built by a centre from the callback that undoes the subscription; nothing else builds one.
class UserNotificationSubscription
{
  public:
    /// An empty subscription, which releases nothing.
    UserNotificationSubscription() noexcept = default;

    /// @param unsubscribe Undoes the subscription; run once, when this is released or destroyed.
    explicit UserNotificationSubscription(std::function<void()> unsubscribe) noexcept:
        _unsubscribe { std::move(unsubscribe) }
    {
    }

    UserNotificationSubscription(UserNotificationSubscription const&) = delete;
    UserNotificationSubscription& operator=(UserNotificationSubscription const&) = delete;

    UserNotificationSubscription(UserNotificationSubscription&& other) noexcept:
        _unsubscribe { std::exchange(other._unsubscribe, nullptr) }
    {
    }

    UserNotificationSubscription& operator=(UserNotificationSubscription&& other) noexcept
    {
        if (this != &other)
        {
            release();
            _unsubscribe = std::exchange(other._unsubscribe, nullptr);
        }
        return *this;
    }

    ~UserNotificationSubscription() { release(); }

    /// Unsubscribes now; later clicks for the prefix reach nobody. Safe to call twice.
    void release() noexcept
    {
        if (auto const unsubscribe = std::exchange(_unsubscribe, nullptr); unsubscribe)
            unsubscribe();
    }

  private:
    std::function<void()> _unsubscribe;
};

/// The application-wide notification centre.
///
/// post() and withdraw() are `@pre` main thread and never block.
class IUserNotificationCenter
{
  public:
    IUserNotificationCenter() = default;
    IUserNotificationCenter(IUserNotificationCenter const&) = delete;
    IUserNotificationCenter& operator=(IUserNotificationCenter const&) = delete;
    IUserNotificationCenter(IUserNotificationCenter&&) = delete;
    IUserNotificationCenter& operator=(IUserNotificationCenter&&) = delete;
    virtual ~IUserNotificationCenter() = default;

    /// Raises @p request, replacing a delivered one with the same request id.
    /// @param request The notification.
    virtual void post(UserNotificationRequest request) = 0;

    /// Withdraws the notification carrying @p requestId, delivered or still pending.
    /// @param requestId The id it was posted under.
    virtual void withdraw(std::string const& requestId) = 0;

    /// Routes clicks on notifications whose request id starts with @p prefix to @p onActivated.
    /// @param prefix What the request ids of the subscriber start with.
    /// @param onActivated Runs on the main thread with the id after the prefix.
    /// @param onUndelivered Runs on the main thread with a request the centre accepted but then found it
    ///                      cannot raise (the system refused to register the application while the
    ///                      request waited), so the subscriber raises it another way.
    /// @return The claim; releasing it ends the subscription.
    [[nodiscard]] virtual UserNotificationSubscription subscribe(
        std::string prefix,
        UserNotificationActivated onActivated,
        UserNotificationUndelivered onUndelivered) = 0;

    /// @return Whether post() raises anything.
    [[nodiscard]] virtual UserNotificationAvailability availability() const noexcept = 0;
};

/// The centre where there is none: raises nothing and holds no subscriber.
class NullUserNotificationCenter final: public IUserNotificationCenter
{
  public:
    void post(UserNotificationRequest /*request*/) override {}
    void withdraw(std::string const& /*requestId*/) override {}

    [[nodiscard]] UserNotificationSubscription subscribe(
        std::string /*prefix*/,
        UserNotificationActivated /*onActivated*/,
        UserNotificationUndelivered /*onUndelivered*/) override
    {
        return {};
    }

    [[nodiscard]] UserNotificationAvailability availability() const noexcept override
    {
        return UserNotificationAvailability::Unavailable;
    }
};

/// What every session's request id starts with, before the session's id.
inline constexpr std::string_view SessionRequestPrefix = "contour-session-";

/// The prefix of session @p sessionId's request ids, which two sessions never share: the closing slash
/// keeps session 7's prefix from being the start of session 70's.
/// @param sessionId The session's id.
/// @return For example `contour-session-7/`.
[[nodiscard]] inline std::string sessionRequestPrefix(int sessionId)
{
    return std::format("{}{}/", SessionRequestPrefix, sessionId);
}

/// The prefix of the application's own request ids (a GPU fallback notice), which no session's can be.
inline constexpr std::string_view ApplicationRequestPrefix = "contour-app/";

/// The subscribers of a centre and the routing of a click to the one whose prefix it carries.
///
/// What a real centre and a test double share, so the routing is the same code in both. Main thread only.
class UserNotificationRoutes
{
  public:
    /// Subscribes @p onActivated to the clicks carrying @p prefix.
    /// @param routes The table; the subscription refers to it weakly, so it may go first.
    /// @param prefix What the request ids of the subscriber start with.
    /// @param onActivated Called with the id after the prefix.
    /// @param onUndelivered Called with a request of this prefix that could not be raised.
    /// @return The claim that removes the subscriber when released.
    [[nodiscard]] static UserNotificationSubscription subscribe(
        std::shared_ptr<UserNotificationRoutes> const& routes,
        std::string prefix,
        UserNotificationActivated onActivated,
        UserNotificationUndelivered onUndelivered)
    {
        auto const key = routes->_nextKey++;
        routes->_subscribers.emplace(key,
                                     Subscriber { .prefix = std::move(prefix),
                                                  .onActivated = std::move(onActivated),
                                                  .onUndelivered = std::move(onUndelivered) });
        return UserNotificationSubscription { [weak = std::weak_ptr { routes }, key] {
            if (auto const table = weak.lock())
                table->_subscribers.erase(key);
        } };
    }

    /// Delivers a click on @p requestId to the subscriber whose prefix it starts with.
    /// @param requestId The id of the notification that was clicked.
    /// @return Whether a subscriber took it; a click nobody claims is dropped.
    [[nodiscard]] bool route(std::string_view requestId) const
    {
        for (auto const& [key, subscriber]: _subscribers)
        {
            if (!requestId.starts_with(subscriber.prefix))
                continue;
            // A copy: the callback may release its own subscription.
            auto const onActivated = subscriber.onActivated;
            onActivated(requestId.substr(subscriber.prefix.size()));
            return true;
        }
        return false;
    }

    /// Hands @p request, which could not be raised, back to the subscriber whose prefix it starts with.
    /// @param request The request that was held and cannot be raised.
    /// @return Whether a subscriber took it; one whose owner is gone drops it.
    [[nodiscard]] bool undeliver(UserNotificationRequest const& request) const
    {
        for (auto const& [key, subscriber]: _subscribers)
        {
            if (!request.requestId.starts_with(subscriber.prefix))
                continue;
            // A copy: the callback may release its own subscription.
            auto const onUndelivered = subscriber.onUndelivered;
            onUndelivered(request);
            return true;
        }
        return false;
    }

    /// @return How many subscribers there are.
    [[nodiscard]] std::size_t size() const noexcept { return _subscribers.size(); }

  private:
    struct Subscriber
    {
        std::string prefix;
        UserNotificationActivated onActivated;
        UserNotificationUndelivered onUndelivered;
    };

    std::map<std::size_t, Subscriber> _subscribers {};
    std::size_t _nextKey = 0;
};

/// What the user has told the operating system about this application's notifications.
enum class AuthorizationState : uint8_t
{
    Unasked = 0, ///< The user was never asked.
    Pending,     ///< The prompt is up, unanswered.
    Granted,     ///< Notifications may be raised.
    Denied,      ///< The user refused.
};

/// What to do with a request, given the authorization.
enum class GateAction : uint8_t
{
    Ask = 0, ///< Ask for authorization now; the request is held until it is answered.
    Hold,    ///< The prompt is up; the request is held until it is answered.
    Send,    ///< Raise it.
    Drop,    ///< Raise nothing.
};

/// What the system's notification settings say, as the adapter reads them.
enum class SystemAuthorization : uint8_t
{
    NotDetermined = 0, ///< The user was not asked yet.
    Authorized,        ///< Notifications may be raised (a provisional grant counts).
    Denied,            ///< The user refused.
};

/// How the system answered a request for authorization.
enum class AuthorizationAnswer : uint8_t
{
    Refused = 0, ///< The user said no.
    Granted,     ///< The user said yes.
    Failed,      ///< The system could not take the request at all, which is not the user's refusal.
};

/// Where requests the gate lets go of are to be raised.
enum class ReleaseTo : uint8_t
{
    Centre = 0, ///< On the notification centre.
    Fallback,   ///< Through the subscribers' own fallback, the centre having become unavailable.
};

/// The requests the gate lets go of, and where they go.
struct GateRelease
{
    std::vector<UserNotificationRequest> requests {}; ///< Oldest first; empty when nothing was held.
    ReleaseTo destination = ReleaseTo::Centre;        ///< Where to raise them.
};

/// How many requests wait for the prompt's answer; the oldest go first when more arrive.
inline constexpr std::size_t AuthorizationHoldLimit = 16;

/// The decision of whether a post asks, waits, goes out or is dropped, and the queue while the user
/// answers the prompt. Pure, so it is tested without an operating system to ask.
class AuthorizationGate
{
  public:
    /// @param start What the system already says, read once at construction.
    explicit AuthorizationGate(AuthorizationState start) noexcept: _state { start } {}

    /// Decides what a post of @p request does. Asking and holding keep a copy of the request; one that
    /// replaces a held request with the same id takes its place, so a session's notices do not pile up.
    /// @param request The notification about to be raised.
    /// @return What the caller does next.
    [[nodiscard]] GateAction admit(UserNotificationRequest const& request)
    {
        switch (_state)
        {
            case AuthorizationState::Unasked:
                _state = AuthorizationState::Pending;
                hold(request);
                return GateAction::Ask;
            case AuthorizationState::Pending: hold(request); return GateAction::Hold;
            case AuthorizationState::Granted: return GateAction::Send;
            case AuthorizationState::Denied: return GateAction::Drop;
        }
        return GateAction::Drop;
    }

    /// Records the user's answer to the prompt.
    /// @param answer Granted or Denied; any other value is not an answer and changes nothing.
    /// @return The requests held meanwhile, oldest first, to raise now; none when refused.
    [[nodiscard]] std::vector<UserNotificationRequest> answer(AuthorizationState answer)
    {
        if (answer != AuthorizationState::Granted && answer != AuthorizationState::Denied)
            return {};
        _state = answer;
        auto held = std::exchange(_held, {});
        if (answer == AuthorizationState::Denied)
            return {};
        return held;
    }

    /// Drops the held request @p requestId, so a session closed while the prompt is up never posts after it.
    /// @param requestId The id the request was admitted under.
    void forget(std::string const& requestId)
    {
        std::erase_if(_held, [&](auto const& held) { return held.requestId == requestId; });
    }

    /// Takes what the system's notification settings say, which the adapter reads asynchronously after
    /// construction and which may arrive while a request for authorization is in flight.
    /// @param reported The settings' authorization.
    /// @return The held requests to raise now when it is Authorized; none otherwise. NotDetermined
    ///         changes nothing: the prompt, if up, is still unanswered.
    [[nodiscard]] GateRelease settingsReported(SystemAuthorization reported)
    {
        switch (reported)
        {
            case SystemAuthorization::NotDetermined: return {};
            case SystemAuthorization::Authorized:
                return GateRelease { .requests = answer(AuthorizationState::Granted) };
            case SystemAuthorization::Denied:
                return GateRelease { .requests = answer(AuthorizationState::Denied) };
        }
        return {};
    }

    /// Takes the system's answer to the request for authorization. A failure turns the gate unavailable
    /// for good and hands what waited to the fallback, so a notice is never silently lost.
    /// @param result How the system answered.
    /// @return The held requests and where to raise them; none held after a refusal or a second answer.
    [[nodiscard]] GateRelease requestAnswered(AuthorizationAnswer result)
    {
        switch (result)
        {
            case AuthorizationAnswer::Refused:
                return GateRelease { .requests = answer(AuthorizationState::Denied) };
            case AuthorizationAnswer::Granted:
                return GateRelease { .requests = answer(AuthorizationState::Granted) };
            case AuthorizationAnswer::Failed:
                _availability = UserNotificationAvailability::Unavailable;
                _state = AuthorizationState::Denied;
                return GateRelease { .requests = std::exchange(_held, {}),
                                     .destination = ReleaseTo::Fallback };
        }
        return {};
    }

    /// @return The authorization as the gate knows it.
    [[nodiscard]] AuthorizationState state() const noexcept { return _state; }

    /// @return Unavailable once the system could not take a request for authorization at all.
    [[nodiscard]] UserNotificationAvailability availability() const noexcept { return _availability; }

  private:
    void hold(UserNotificationRequest const& request)
    {
        if (auto const same =
                std::ranges::find(_held, request.requestId, &UserNotificationRequest::requestId);
            same != _held.end())
        {
            *same = request;
            return;
        }
        if (_held.size() == AuthorizationHoldLimit)
            _held.erase(_held.begin());
        _held.push_back(request);
    }

    AuthorizationState _state;
    UserNotificationAvailability _availability = UserNotificationAvailability::Available;
    std::vector<UserNotificationRequest> _held {};
};

/// A centre that builds the real one on first use, so a process that never raises a notification (every
/// `contour` verb but the GUI) never touches the operating system's centre, which the construction of
/// some does with side effects on what an earlier run left there.
///
/// Building happens on the first post(), subscribe() or availability(); withdraw() of something never
/// posted builds nothing. Main thread only, like the centre it wraps.
class LazyUserNotificationCenter final: public IUserNotificationCenter
{
  public:
    /// @param factory Builds the centre; run once, on first use, and must return a non-null centre.
    explicit LazyUserNotificationCenter(std::function<std::unique_ptr<IUserNotificationCenter>()> factory):
        _factory { std::move(factory) }
    {
    }

    void post(UserNotificationRequest request) override { built().post(std::move(request)); }

    void withdraw(std::string const& requestId) override
    {
        if (_center)
            _center->withdraw(requestId);
    }

    [[nodiscard]] UserNotificationSubscription subscribe(std::string prefix,
                                                         UserNotificationActivated onActivated,
                                                         UserNotificationUndelivered onUndelivered) override
    {
        return built().subscribe(std::move(prefix), std::move(onActivated), std::move(onUndelivered));
    }

    [[nodiscard]] UserNotificationAvailability availability() const noexcept override
    {
        return built().availability();
    }

    /// @return Whether the centre was built yet.
    [[nodiscard]] bool isBuilt() const noexcept { return _center != nullptr; }

  private:
    [[nodiscard]] IUserNotificationCenter& built() const
    {
        if (!_center)
            _center = _factory();
        return *_center;
    }

    std::function<std::unique_ptr<IUserNotificationCenter>()> _factory;
    mutable std::unique_ptr<IUserNotificationCenter> _center {};
};

/// @return Whether this process runs from an application bundle with an identifier; always Absent off
///         Apple. Reads the main bundle, so it belongs to the adapter, not to the decision.
[[nodiscard]] AppBundle currentAppBundle();

/// The notification centre this process can use: Notification Center on macOS when running from an
/// application bundle with an identifier, and the null centre everywhere else, including a bare binary
/// and every test run. The class behind it is never touched unless the bundle is present.
/// @pre Main thread, with the application object constructed.
/// @return The centre; never null.
[[nodiscard]] std::unique_ptr<IUserNotificationCenter> makeUserNotificationCenter();

} // namespace contour::platform
