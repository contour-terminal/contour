// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/UserNotificationCenter.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace contour::test
{

/// A notification centre that records what it was asked and routes a click as the real one does.
class RecordingUserNotificationCenter final: public contour::platform::IUserNotificationCenter
{
  public:
    /// @param availability What availability() answers.
    explicit RecordingUserNotificationCenter(
        contour::platform::UserNotificationAvailability availability =
            contour::platform::UserNotificationAvailability::Available) noexcept:
        _availability { availability }
    {
    }

    void post(contour::platform::UserNotificationRequest request) override
    {
        posted.push_back(std::move(request));
    }

    void withdraw(std::string const& requestId) override { withdrawn.push_back(requestId); }

    [[nodiscard]] contour::platform::UserNotificationSubscription subscribe(
        std::string prefix,
        contour::platform::UserNotificationActivated onActivated,
        contour::platform::UserNotificationUndelivered onUndelivered) override
    {
        return contour::platform::UserNotificationRoutes::subscribe(
            _routes, std::move(prefix), std::move(onActivated), std::move(onUndelivered));
    }

    [[nodiscard]] contour::platform::UserNotificationAvailability availability() const noexcept override
    {
        return _availability;
    }

    /// Changes what availability() answers, as a refused authorization does.
    /// @param availability The new answer.
    void setAvailability(contour::platform::UserNotificationAvailability availability) noexcept
    {
        _availability = availability;
    }

    /// Plays back a click on the notification posted as @p requestId.
    /// @param requestId The request id of the clicked notification.
    /// @return Whether a subscriber took the click.
    [[nodiscard]] bool activate(std::string const& requestId) const { return _routes->route(requestId); }

    /// Plays back the centre giving up on @p request, as a failed authorization does for what it held.
    /// @param request The request that cannot be raised.
    /// @return Whether a subscriber took it.
    [[nodiscard]] bool undeliver(contour::platform::UserNotificationRequest const& request) const
    {
        return _routes->undeliver(request);
    }

    /// @return How many subscribers are live.
    [[nodiscard]] std::size_t subscriberCount() const noexcept { return _routes->size(); }

    std::vector<contour::platform::UserNotificationRequest> posted {}; ///< Every post, in order.
    std::vector<std::string> withdrawn {};                             ///< Every withdrawal, in order.

  private:
    contour::platform::UserNotificationAvailability _availability;
    std::shared_ptr<contour::platform::UserNotificationRoutes> _routes =
        std::make_shared<contour::platform::UserNotificationRoutes>();
};

/// A centre that forwards to another it does not own, so an app can own one while a test keeps and reads
/// the recording centre behind it.
class ForwardingUserNotificationCenter final: public contour::platform::IUserNotificationCenter
{
  public:
    /// @param target The centre to forward to; it must outlive this one.
    explicit ForwardingUserNotificationCenter(contour::platform::IUserNotificationCenter& target) noexcept:
        _target { target }
    {
    }

    void post(contour::platform::UserNotificationRequest request) override
    {
        _target.post(std::move(request));
    }

    void withdraw(std::string const& requestId) override { _target.withdraw(requestId); }

    [[nodiscard]] contour::platform::UserNotificationSubscription subscribe(
        std::string prefix,
        contour::platform::UserNotificationActivated onActivated,
        contour::platform::UserNotificationUndelivered onUndelivered) override
    {
        return _target.subscribe(std::move(prefix), std::move(onActivated), std::move(onUndelivered));
    }

    [[nodiscard]] contour::platform::UserNotificationAvailability availability() const noexcept override
    {
        return _target.availability();
    }

  private:
    contour::platform::IUserNotificationCenter& _target;
};

/// @p services with @p centre as the application's notification centre, and @p host as the host the app
/// believes it runs on, so the macOS notification path is reachable on any OS.
/// @param services The services to adapt, typically from appearanceServicesWith() or the null ones.
/// @param centre The centre; it must outlive the app built from the result.
/// @param host The host the app should report.
/// @return The adapted services.
[[nodiscard]] inline contour::platform::AppearanceServices withUserNotifications(
    contour::platform::AppearanceServices services,
    contour::platform::IUserNotificationCenter& centre,
    contour::config::HostPlatform host)
{
    services.userNotifications = [&centre] {
        return std::make_unique<ForwardingUserNotificationCenter>(centre);
    };
    services.hostPlatform = host;
    return services;
}

} // namespace contour::test
