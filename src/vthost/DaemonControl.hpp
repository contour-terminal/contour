// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a native connection asks of the daemon behind it: its status, a stop, and a census ticket for every
/// client. An endpoint without a daemon behind it -- a test, an embedded host -- is served by the null
/// object, which answers every question NotServed and counts no one.

#include <core/async/Task.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>

#include <vthost/ClientCensus.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/handover/UpgradeError.hpp>
#include <vthost/handover/UpgradeTarget.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Why a daemon-control request was not served.
enum class DaemonControlError : uint8_t
{
    NotServed = 0, ///< This endpoint has no daemon behind it (tests, embedded hosts).
};

/// A connection the daemon tells that it is about to replace itself with another release: first that it is
/// (so the peer waits for the same daemon to come back), then to end, in an orderly way of its own.
class UpgradeObserver
{
  public:
    virtual ~UpgradeObserver() = default;

    /// The daemon is upgrading: the connection is closed shortly, and the same daemon comes back.
    /// @param toRelease The release it becomes.
    virtual void announceUpgrade(std::string_view toRelease) = 0;

    /// Ends the connection now, having flushed what is queued for the peer (DaemonUpgrading among it).
    virtual void closeForUpgrade() = 0;

    /// Closes the connection at once, whatever is still queued for the peer: for one that did not end after
    /// closeForUpgrade(), as a peer that stopped reading would not.
    virtual void abortForUpgrade() = 0;
};

/// The daemon as its connections see it.
class DaemonControl
{
  public:
    virtual ~DaemonControl() = default;

    /// @return The daemon's status, as a DaemonStatusReply states it.
    [[nodiscard]] virtual std::expected<proto::DaemonStatusReply, DaemonControlError> status() const = 0;

    /// @return The daemon's release, which every accepted ServerHello carries; empty when unknown.
    [[nodiscard]] virtual std::string_view releaseVersion() const noexcept = 0;

    /// Asks the daemon to stop.
    /// @param reason Why.
    /// @return Nothing once the stop is under way.
    [[nodiscard]] virtual std::expected<void, DaemonControlError> requestStop(proto::StopReason reason) = 0;

    /// Counts one more client, for as long as the returned ticket lives.
    /// @return The ticket.
    [[nodiscard]] virtual ClientCensus::Ticket admitClient() = 0;

    /// Upgrades the daemon in place to @p target (live upgrade). Loop thread.
    ///
    /// A coroutine, because the upgrade waits on the loop. On success it never completes: the process is the
    /// new image.
    /// @param target What to upgrade to.
    /// @param onStarted Called once, when the upgrade is certain to proceed and just before every client is
    ///        told and every connection closed: the last moment the requester can still be answered.
    /// @return Why the upgrade did not happen; the daemon serves on as before.
    [[nodiscard]] virtual core::async::Task<std::expected<void, handover::UpgradeError>> requestUpgrade(
        handover::UpgradeTarget target, std::function<void()> onStarted) = 0;

    /// Has @p observer told whenever the daemon announces an upgrade, until unsubscribeUpgrade().
    /// @param observer Not owned; must stay alive until unsubscribed.
    virtual void subscribeUpgrade(UpgradeObserver& observer) = 0;

    /// Stops telling @p observer; harmless when it is not subscribed.
    /// @param observer A subscriber, or not.
    virtual void unsubscribeUpgrade(UpgradeObserver& observer) = 0;
};

/// A subscription to a daemon's upgrade announcements, for as long as it lives.
class UpgradeSubscription
{
  public:
    /// @param control The daemon to listen to (not owned; outlives this).
    /// @param observer Told of every announcement (not owned; outlives this).
    UpgradeSubscription(DaemonControl& control, UpgradeObserver& observer):
        _control(control), _observer(observer)
    {
        _control.subscribeUpgrade(_observer);
    }
    UpgradeSubscription(UpgradeSubscription const&) = delete;
    UpgradeSubscription& operator=(UpgradeSubscription const&) = delete;
    UpgradeSubscription(UpgradeSubscription&&) = delete;
    UpgradeSubscription& operator=(UpgradeSubscription&&) = delete;
    ~UpgradeSubscription() { _control.unsubscribeUpgrade(_observer); }

  private:
    DaemonControl& _control;
    UpgradeObserver& _observer;
};

/// The daemon control of an endpoint with no daemon behind it: an empty release, NotServed for status and
/// stop, NotSupported for an upgrade, and an empty ticket for every client.
class NullDaemonControl final: public DaemonControl
{
  public:
    [[nodiscard]] std::expected<proto::DaemonStatusReply, DaemonControlError> status() const override;
    [[nodiscard]] std::string_view releaseVersion() const noexcept override;
    [[nodiscard]] std::expected<void, DaemonControlError> requestStop(proto::StopReason reason) override;
    [[nodiscard]] ClientCensus::Ticket admitClient() override;
    [[nodiscard]] core::async::Task<std::expected<void, handover::UpgradeError>> requestUpgrade(
        handover::UpgradeTarget target, std::function<void()> onStarted) override;
    void subscribeUpgrade(UpgradeObserver& observer) override;
    void unsubscribeUpgrade(UpgradeObserver& observer) override;
};

/// The one NullDaemonControl. It holds no state at all, so every connection of every host may share it, from
/// any thread.
/// @return It, by reference: there is no nullable DaemonControl pointer anywhere.
[[nodiscard]] DaemonControl& nullDaemonControl() noexcept;

/// Wraps @p handler so that each connection it serves holds a census ticket of @p control's for its whole
/// life: for the endpoints whose every connection is a client (tmux control mode, imsg).
/// @param control The daemon that counts the connections; outlives the returned handler.
/// @param handler The connection handler to wrap.
/// @return The counting handler.
[[nodiscard]] ConnectionHandler countedHandler(DaemonControl& control, ConnectionHandler handler);

} // namespace vthost
