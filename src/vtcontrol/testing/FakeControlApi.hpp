// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <vtcontrol/ControlApi.hpp>

namespace vtcontrol::testing
{

/// An `EventSink` that remembers what it was given.
struct RecordingEventSink final: public EventSink
{
    /// One delivery.
    struct Delivery
    {
        uint32_t subscription = 0; ///< The subscription.
        Event event;               ///< The event.
    };

    void deliverEvent(uint32_t subscription, Event const& event) override
    {
        deliveries.push_back(Delivery { .subscription = subscription, .event = event });
    }

    void subscriptionEnded(uint32_t subscription) override { ended.push_back(subscription); }

    std::vector<Delivery> deliveries; ///< Everything delivered so far.
    std::vector<uint32_t> ended;      ///< The subscriptions ended so far, in order.
};

/// A `CallContext` with a settable caller and grants, and a recording sink.
struct FakeCallContext final: public CallContext
{
    [[nodiscard]] std::string_view caller() const noexcept override { return callerPane; }
    [[nodiscard]] EventSink& events() noexcept override { return sink; }
    [[nodiscard]] ControlGrants grants() const noexcept override { return granted; }

    std::string callerPane;                       ///< What `caller()` returns.
    RecordingEventSink sink;                      ///< What `events()` returns.
    ControlGrants granted = ControlGrants::all(); ///< What `grants()` returns.
};

/// A `ControlApi` for tests: records the last request of each type and answers with the configured reply.
struct FakeControlApi final: public ControlApi
{
    // clang-format off
    void handle(CallContext& /*context*/, DaemonInfoRequest const& request, Completion<DaemonInfoReply> done) override { lastDaemonInfo = request; done(daemonInfoReply); }
    void handle(CallContext& /*context*/, ListRequest const& request, Completion<ListReply> done) override { lastList = request; done(listReply); }
    void handle(CallContext& /*context*/, CreateTabRequest const& request, Completion<CreateTabReply> done) override { lastCreateTab = request; done(createTabReply); }
    void handle(CallContext& /*context*/, SplitRequest const& request, Completion<SplitReply> done) override { lastSplit = request; done(splitReply); }
    void handle(CallContext& /*context*/, SendRequest const& request, Completion<SendReply> done) override { lastSend = request; done(sendReply); }
    void handle(CallContext& /*context*/, CaptureRequest const& request, Completion<CaptureReply> done) override { lastCapture = request; done(captureReply); }
    void handle(CallContext& /*context*/, ClosePaneRequest const& request, Completion<EmptyReply> done) override { lastClosePane = request; done(EmptyReply {}); }
    void handle(CallContext& /*context*/, CloseTabRequest const& request, Completion<EmptyReply> done) override { lastCloseTab = request; if (closeTabFailure) done(std::unexpected(*closeTabFailure)); else done(EmptyReply {}); }
    void handle(CallContext& /*context*/, SetLabelRequest const& request, Completion<EmptyReply> done) override { lastSetLabel = request; done(EmptyReply {}); }
    void handle(CallContext& /*context*/, BlockListRequest const& request, Completion<BlockListReply> done) override { lastBlockList = request; done(blockListReply); }
    void handle(CallContext& /*context*/, BlockReadRequest const& request, Completion<BlockReadReply> done) override { lastBlockRead = request; done(blockReadReply); }
    void handle(CallContext& /*context*/, WaitRequest const& request, Completion<WaitReply> done) override { lastWait = request; done(waitReply); }
    void handle(CallContext& /*context*/, SubscribeRequest const& request, Completion<SubscribeReply> done) override { lastSubscribe = request; done(subscribeReply); }
    void handle(CallContext& /*context*/, UnsubscribeRequest const& request, Completion<EmptyReply> done) override { lastUnsubscribe = request; done(EmptyReply {}); }
    // clang-format on

    void clientAttached(std::string_view client) override { attached.emplace_back(client); }
    void clientDetached(std::string_view client) override { detached.emplace_back(client); }
    void sinkClosed(EventSink& sink) override { closedSinks.push_back(&sink); }

    FakeCallContext context; ///< A context to pass to `dispatch`.

    DaemonInfoReply daemonInfoReply;
    ListReply listReply;
    CreateTabReply createTabReply;
    SplitReply splitReply;
    SendReply sendReply;
    CaptureReply captureReply;
    BlockListReply blockListReply;
    BlockReadReply blockReadReply;
    WaitReply waitReply;
    SubscribeReply subscribeReply;
    std::optional<ControlError> closeTabFailure; ///< When set, `tab.close` fails with it.

    std::optional<DaemonInfoRequest> lastDaemonInfo;
    std::optional<ListRequest> lastList;
    std::optional<CreateTabRequest> lastCreateTab;
    std::optional<SplitRequest> lastSplit;
    std::optional<SendRequest> lastSend;
    std::optional<CaptureRequest> lastCapture;
    std::optional<ClosePaneRequest> lastClosePane;
    std::optional<CloseTabRequest> lastCloseTab;
    std::optional<SetLabelRequest> lastSetLabel;
    std::optional<BlockListRequest> lastBlockList;
    std::optional<BlockReadRequest> lastBlockRead;
    std::optional<WaitRequest> lastWait;
    std::optional<SubscribeRequest> lastSubscribe;
    std::optional<UnsubscribeRequest> lastUnsubscribe;

    std::vector<std::string> attached;
    std::vector<std::string> detached;
    std::vector<EventSink*> closedSinks;
};

} // namespace vtcontrol::testing
