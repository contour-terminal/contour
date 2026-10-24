// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/Events.hpp>

namespace vtcontrol
{

/// How a handler reports the outcome of a call; it may be invoked synchronously or later, once.
/// @tparam Reply The reply type of the method.
template <typename Reply>
using Completion = std::move_only_function<void(std::expected<Reply, ControlError>)>;

/// Where a subscription's events are delivered. One sink is one connection (or one extension).
class EventSink
{
  public:
    EventSink() = default;
    EventSink(EventSink const&) = delete;
    EventSink(EventSink&&) = delete;
    EventSink& operator=(EventSink const&) = delete;
    EventSink& operator=(EventSink&&) = delete;
    virtual ~EventSink() = default;

    /// Hands one event to the subscriber.
    /// @param subscription The id `events.subscribe` returned.
    /// @param event The event.
    virtual void deliverEvent(uint32_t subscription, Event const& event) = 0;

    /// A subscription of this sink ended by `events.unsubscribe`: forget what is kept for it. Not called when
    /// the sink itself goes (`ControlApi::sinkClosed`).
    /// @param subscription The id that ended.
    virtual void subscriptionEnded(uint32_t /*subscription*/) {}
};

/// What a handler knows about the call it is serving.
///
/// Call-scoped, like the request: the transport builds it on its stack for one `dispatch`, and `caller()`
/// views the request's bytes. A handler that completes later copies what it needs during the call -- the
/// `EventSink&` from `events()` is the exception, which lives as long as its connection and is dropped
/// through `ControlApi::sinkClosed` before it goes.
class CallContext
{
  public:
    CallContext() = default;
    CallContext(CallContext const&) = delete;
    CallContext(CallContext&&) = delete;
    CallContext& operator=(CallContext const&) = delete;
    CallContext& operator=(CallContext&&) = delete;
    virtual ~CallContext() = default;

    /// @return The caller's pane id (`$CONTOUR_PANE`), or an empty view when the caller is not in a pane.
    [[nodiscard]] virtual std::string_view caller() const noexcept = 0;

    /// @return Where this connection's subscribed events go.
    [[nodiscard]] virtual EventSink& events() noexcept = 0;

    /// @return What the connection is granted; `dispatch` refuses a method outside it as `Forbidden`.
    [[nodiscard]] virtual ControlGrants grants() const noexcept = 0;
};

/// The control API, one `handle` overload per catalog row.
///
/// A request is only valid for the duration of the call: a handler that finishes later copies what it needs.
/// `api.list` and `api.describe` have default bodies that answer from the catalog.
class ControlApi
{
  public:
    ControlApi() = default;
    ControlApi(ControlApi const&) = delete;
    ControlApi(ControlApi&&) = delete;
    ControlApi& operator=(ControlApi const&) = delete;
    ControlApi& operator=(ControlApi&&) = delete;
    virtual ~ControlApi() = default;

    // clang-format off
    virtual void handle(CallContext& context, DaemonInfoRequest const& request, Completion<DaemonInfoReply> done) = 0;
    virtual void handle(CallContext& context, ListRequest const& request, Completion<ListReply> done) = 0;
    virtual void handle(CallContext& context, CreateTabRequest const& request, Completion<CreateTabReply> done) = 0;
    virtual void handle(CallContext& context, SplitRequest const& request, Completion<SplitReply> done) = 0;
    virtual void handle(CallContext& context, SendRequest const& request, Completion<SendReply> done) = 0;
    virtual void handle(CallContext& context, CaptureRequest const& request, Completion<CaptureReply> done) = 0;
    virtual void handle(CallContext& context, ClosePaneRequest const& request, Completion<EmptyReply> done) = 0;
    virtual void handle(CallContext& context, CloseTabRequest const& request, Completion<EmptyReply> done) = 0;
    virtual void handle(CallContext& context, SetLabelRequest const& request, Completion<EmptyReply> done) = 0;
    virtual void handle(CallContext& context, BlockListRequest const& request, Completion<BlockListReply> done) = 0;
    virtual void handle(CallContext& context, BlockReadRequest const& request, Completion<BlockReadReply> done) = 0;
    virtual void handle(CallContext& context, WaitRequest const& request, Completion<WaitReply> done) = 0;
    virtual void handle(CallContext& context, SubscribeRequest const& request, Completion<SubscribeReply> done) = 0;
    virtual void handle(CallContext& context, UnsubscribeRequest const& request, Completion<EmptyReply> done) = 0;
    // clang-format on

    /// `api.list`, answered from the catalog.
    virtual void handle(CallContext& context, ApiListRequest const& request, Completion<ApiListReply> done);

    /// `api.describe`, answered from the catalog.
    virtual void handle(CallContext& context,
                        ApiDescribeRequest const& request,
                        Completion<ApiDescription> done);

    /// A viewer connection completed its handshake. Reported by the transport.
    /// @param client A description of the client.
    virtual void clientAttached(std::string_view client);

    /// That viewer connection ended. Reported by the transport.
    /// @param client A description of the client.
    virtual void clientDetached(std::string_view client);

    /// A sink is going away: drop every subscription and waiter of it.
    /// @param sink The sink.
    virtual void sinkClosed(EventSink& sink);
};

/// A stateless `ControlApi` that serves nothing: every call completes with `Unsupported`
/// ("the control API is not served here"), and the lifecycle calls do nothing. It is the default of
/// `NativeSessionServices::control`; it is returned by reference because it has no state and nothing
/// to configure.
/// @return The null object.
[[nodiscard]] ControlApi& nullControlApi() noexcept;

} // namespace vtcontrol
