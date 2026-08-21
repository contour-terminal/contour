// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The PortalCaller test double, shared by every portal-speaking class's tests.
///
/// PortalCaller was extracted into PortalCall.hpp precisely because two classes speak it; its
/// recorder belongs in one place for the same reason, or the third one writes a third copy.

#ifdef __linux__

    #include <contour/platform/PortalCall.hpp>

    #include <QtCore/QString>
    #include <QtCore/QVariantList>

    #include <cstddef>
    #include <functional>
    #include <utility>
    #include <vector>

namespace contour::test
{

/// One recorded portal method call, with the reply the test has yet to deliver.
struct RecordedPortalCall
{
    QString method;
    QVariantList arguments;
    std::function<void(contour::platform::CallOutcome)> onReply;
};

/// Every portal call that was issued, and nothing answered unasked.
///
/// Deliberately not a mock framework: the recorded vector IS the assertion. Nothing replies on its
/// own, which is how a test observes that a caller returned BEFORE any reply existed -- the property
/// the asynchronous portal seam exists for.
class RecordingPortalCaller
{
  public:
    [[nodiscard]] contour::platform::PortalCaller caller()
    {
        return [this](QObject* /*context*/,
                      QLatin1StringView method,
                      QVariantList const& arguments,
                      std::function<void(contour::platform::CallOutcome)> onReply) {
            calls.emplace_back(RecordedPortalCall { QString(method), arguments, std::move(onReply) });
        };
    }

    /// Delivers the portal's reply for the call at @p index, as the bus would.
    ///
    /// The handler is COPIED out before it runs: a reply handler may issue another portal call --
    /// PortalNotificationTransport's does, withdrawing a notification from inside the reply that
    /// recorded it -- and the emplace_back that records it can reallocate `calls`, destroying the
    /// std::function under its own running call.
    void completeCall(size_t index, contour::platform::CallOutcome outcome)
    {
        auto const onReply = calls.at(index).onReply;
        onReply(outcome);
    }

    std::vector<RecordedPortalCall> calls;
};

} // namespace contour::test

#endif // defined(__linux__)
