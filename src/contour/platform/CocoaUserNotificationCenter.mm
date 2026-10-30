// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/UserNotificationCenter.hpp>

#ifdef __APPLE__

    #include <contour/Logging.hpp>

    #include <core/Assert.hpp>

    #include <cstdint>
    #include <memory>
    #include <string>
    #include <utility>

    #import <Foundation/Foundation.h>
    #import <UserNotifications/UserNotifications.h>

namespace contour::platform
{

namespace
{
    /// @return @p text as an NSString; empty where it is not valid UTF-8.
    [[nodiscard]] NSString* nsString(std::string const& text)
    {
        NSString* const converted = [NSString stringWithUTF8String:text.c_str()];
        return converted != nil ? converted : @"";
    }

    /// Hands @p request to Notification Center, replacing a delivered one with the same identifier.
    /// @pre Main thread; authorization was granted.
    void deliver(UserNotificationRequest const& request)
    {
        UNMutableNotificationContent* const content = [[UNMutableNotificationContent alloc] init];
        content.title = nsString(request.title);
        content.body = nsString(request.body);
        content.threadIdentifier = nsString(request.threadId);
        switch (request.sound)
        {
            case vtbackend::NotificationSound::Silent: content.sound = nil; break;
            case vtbackend::NotificationSound::DesktopDefault:
            case vtbackend::NotificationSound::Chime: content.sound = [UNNotificationSound defaultSound]; break;
        }

        UNNotificationRequest* const native =
            [UNNotificationRequest requestWithIdentifier:nsString(request.requestId)
                                                 content:content
                                                 trigger:nil];
        [[UNUserNotificationCenter currentNotificationCenter]
            addNotificationRequest:native
             withCompletionHandler:^(NSError* _Nullable error) {
               if (error != nil)
                   notifierLog()("Notification Center refused a request: {}",
                                 error.localizedDescription.UTF8String);
             }];
    }

    /// Whether the refusal was told to the log yet; it is once per run.
    enum class RefusalLogged : uint8_t
    {
        No = 0,
        Yes,
    };

    /// What the centre's asynchronous callbacks reach, by weak reference, so one arriving after the
    /// centre is gone finds nothing. Main thread only.
    struct SharedState
    {
        /// @param start What the system already says about authorization.
        /// @param table Where requests that cannot be raised go back to their subscribers.
        SharedState(AuthorizationState start, std::shared_ptr<UserNotificationRoutes> table):
            gate { start }, routes { std::move(table) }
        {
        }

        AuthorizationGate gate;
        std::shared_ptr<UserNotificationRoutes> routes;
        RefusalLogged refusalLogged = RefusalLogged::No;

        /// Raises what the gate let go of, or hands it back to the subscribers' fallbacks.
        /// @param released The gate's release.
        void settle(GateRelease const& released) const
        {
            for (auto const& request: released.requests)
            {
                switch (released.destination)
                {
                    case ReleaseTo::Centre: deliver(request); break;
                    case ReleaseTo::Fallback: (void) routes->undeliver(request); break;
                }
            }
        }
    };

    /// @return What the system's settings say, in the gate's terms.
    /// @param status The settings' authorization status.
    [[nodiscard]] SystemAuthorization systemAuthorizationOf(UNAuthorizationStatus status) noexcept
    {
        // UNAuthorizationStatusEphemeral is App Clip only and does not exist on macOS.
        switch (status)
        {
            case UNAuthorizationStatusAuthorized:
            case UNAuthorizationStatusProvisional: return SystemAuthorization::Authorized;
            case UNAuthorizationStatusDenied: return SystemAuthorization::Denied;
            case UNAuthorizationStatusNotDetermined: return SystemAuthorization::NotDetermined;
        }
        return SystemAuthorization::NotDetermined;
    }
} // namespace

} // namespace contour::platform

/// The centre's delegate: shows a notification while Contour is frontmost, and reports a click.
@interface ContourUserNotificationDelegate: NSObject <UNUserNotificationCenterDelegate>
- (instancetype)initWithRoutes:(std::weak_ptr<contour::platform::UserNotificationRoutes>)routes;
@end

@implementation ContourUserNotificationDelegate
{
    std::weak_ptr<contour::platform::UserNotificationRoutes> _routes;
}

- (instancetype)initWithRoutes:(std::weak_ptr<contour::platform::UserNotificationRoutes>)routes
{
    self = [super init];
    if (self != nil)
        _routes = std::move(routes);
    return self;
}

// "Unfocused" includes another Contour window being key, so a notification is shown while Contour is
// frontmost too, with its sound when the content has one.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void) center;
    (void) notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList
                      | UNNotificationPresentationOptionSound);
}

// Called on a background queue: the click goes to the main queue and is routed there, and does nothing when
// the centre or the session it belongs to is gone.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    (void) center;
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier])
    {
        auto const requestId = std::string { response.notification.request.identifier.UTF8String };
        auto const routes = _routes;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (auto const table = routes.lock())
              (void) table->route(requestId);
        });
    }
    completionHandler();
}
@end

namespace contour::platform
{

namespace
{
    /// Notification Center behind IUserNotificationCenter.
    ///
    /// Authorization is asked on the first post, never at launch; posts made while the prompt is up wait in
    /// the gate and go out when it is granted. @pre Every method: main thread.
    class CocoaUserNotificationCenter final: public IUserNotificationCenter
    {
      public:
        /// Reads the system's authorization, removes what a previous run left in Notification Center, and
        /// installs the delegate.
        CocoaUserNotificationCenter():
            _routes { std::make_shared<UserNotificationRoutes>() },
            _state { std::make_shared<SharedState>(AuthorizationState::Unasked, _routes) },
            _delegate { [[ContourUserNotificationDelegate alloc]
                initWithRoutes:std::weak_ptr<UserNotificationRoutes> { _routes }] }
        {
            Require(NSThread.isMainThread);
            UNUserNotificationCenter* const center = [UNUserNotificationCenter currentNotificationCenter];
            center.delegate = _delegate;
            // The sessions a previous run raised these for are gone.
            [center removeAllDeliveredNotifications];

            // The answer arrives off the main queue; until it does the gate asks, which the system answers
            // at once when the user already decided.
            auto const weak = std::weak_ptr<SharedState> { _state };
            [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
              auto const reported = systemAuthorizationOf(settings.authorizationStatus);
              dispatch_async(dispatch_get_main_queue(), ^{
                if (auto const state = weak.lock())
                    state->settle(state->gate.settingsReported(reported));
              });
            }];
        }

        CocoaUserNotificationCenter(CocoaUserNotificationCenter const&) = delete;
        CocoaUserNotificationCenter& operator=(CocoaUserNotificationCenter const&) = delete;
        CocoaUserNotificationCenter(CocoaUserNotificationCenter&&) = delete;
        CocoaUserNotificationCenter& operator=(CocoaUserNotificationCenter&&) = delete;

        ~CocoaUserNotificationCenter() override
        {
            // The delegate is held weakly by the system; leave no dangling one behind.
            [UNUserNotificationCenter currentNotificationCenter].delegate = nil;
        }

        void post(UserNotificationRequest request) override
        {
            Require(NSThread.isMainThread);
            switch (_state->gate.admit(request))
            {
                case GateAction::Ask: askForAuthorization(); break;
                case GateAction::Hold: break;
                case GateAction::Send: deliver(request); break;
                case GateAction::Drop:
                    if (std::exchange(_state->refusalLogged, RefusalLogged::Yes) == RefusalLogged::No)
                        notifierLog()("Notifications are refused for Contour; none will be raised "
                                      "(the tab badge still marks a finished command)");
                    break;
            }
        }

        void withdraw(std::string const& requestId) override
        {
            Require(NSThread.isMainThread);
            _state->gate.forget(requestId);
            UNUserNotificationCenter* const center = [UNUserNotificationCenter currentNotificationCenter];
            NSArray<NSString*>* const identifiers = @[ nsString(requestId) ];
            [center removePendingNotificationRequestsWithIdentifiers:identifiers];
            [center removeDeliveredNotificationsWithIdentifiers:identifiers];
        }

        [[nodiscard]] UserNotificationSubscription subscribe(
            std::string prefix,
            UserNotificationActivated onActivated,
            UserNotificationUndelivered onUndelivered) override
        {
            return UserNotificationRoutes::subscribe(
                _routes, std::move(prefix), std::move(onActivated), std::move(onUndelivered));
        }

        /// Unavailable once the system refused to register this application at all, so callers keep the
        /// tray for the rest of the run; what waited for the prompt then goes to their fallbacks.
        [[nodiscard]] UserNotificationAvailability availability() const noexcept override
        {
            return _state->gate.availability();
        }

      private:
        /// Asks the system for permission; its answer is handled on the main queue.
        void askForAuthorization()
        {
            auto const weak = std::weak_ptr<SharedState> { _state };
            [[UNUserNotificationCenter currentNotificationCenter]
                requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                              completionHandler:^(BOOL granted, NSError* _Nullable error) {
                                // An error is not the user's refusal: an ad-hoc-signed build the system
                                // will not register fails here, and keeps the tray.
                                auto const failed = error != nil;
                                if (failed)
                                    notifierLog()("Notification authorization failed: {}",
                                                  error.localizedDescription.UTF8String);
                                auto const result = failed    ? AuthorizationAnswer::Failed
                                                    : granted ? AuthorizationAnswer::Granted
                                                              : AuthorizationAnswer::Refused;
                                dispatch_async(dispatch_get_main_queue(), ^{
                                  if (auto const state = weak.lock())
                                      state->settle(state->gate.requestAnswered(result));
                                });
                              }];
        }

        std::shared_ptr<UserNotificationRoutes> _routes;
        std::shared_ptr<SharedState> _state;
        ContourUserNotificationDelegate* _delegate;
    };
} // namespace

AppBundle currentAppBundle()
{
    NSBundle* const bundle = NSBundle.mainBundle;
    NSString* const path = bundle.bundlePath;
    NSString* const identifier = bundle.bundleIdentifier;
    return appBundleOf(path != nil ? path.UTF8String : "", identifier != nil ? identifier.UTF8String : "");
}

std::unique_ptr<IUserNotificationCenter> makeUserNotificationCenter()
{
    // The class raises outside an application bundle, so nothing below touches it unless one is present.
    if (currentAppBundle() == AppBundle::Absent)
        return std::make_unique<NullUserNotificationCenter>();
    return std::make_unique<CocoaUserNotificationCenter>();
}

} // namespace contour::platform

#endif
