// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ControlService` — the daemon's control API, served over its `SessionHost`.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/ControlApi.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/Target.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/control/BlockWaiters.hpp>
#include <vthost/control/EventHub.hpp>
#include <vtworkspace/ModelEvents.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost::control
{

/// The most event subscriptions one connection may hold at once, by default.
inline constexpr std::size_t DefaultMaxSubscriptionsPerConnection = 64;

/// The most `block.wait`s and `pane.send` waits one connection may have pending at once, by default.
inline constexpr std::size_t DefaultMaxWaitsPerConnection = 256;

/// How a ControlService is set up. Fixed at construction.
struct ControlServiceConfig
{
    std::string version;              ///< The daemon's release version, as `daemon.info` reports it.
    std::filesystem::path socketPath; ///< The daemon's control socket.
    /// What uptimes are read from. Borrowed: it must outlive the service.
    core::platform::WallClockRef clock;
    /// The most bytes of pane text one reply carries; longer text keeps its tail.
    std::size_t maxReplyBytes = std::size_t { 1 } * 1024 * 1024;
    /// The most event subscriptions one connection may hold at once; one more is refused `LimitReached`.
    /// Bounds what a connection that subscribes in a loop costs the daemon, in memory and per event.
    std::size_t maxSubscriptionsPerConnection = DefaultMaxSubscriptionsPerConnection;
    /// The most waits one connection may have pending at once; one more is refused `LimitReached`. Bounds
    /// what a connection that pipelines waits without reading their replies costs the daemon.
    std::size_t maxWaitsPerConnection = DefaultMaxWaitsPerConnection;
};

/// Implements the control API over a SessionHost: one method per catalog row.
///
/// An API pane is one hosted session, named by the UUID the host's ObjectRegistry keeps for it. A target that
/// names no live object is `NotFound`, never a fallback to another one.
///
/// It observes the host's sessions and model for the events it publishes and the waits it completes.
///
/// Threading: loop thread only, like the host. What a session's parser raises (a finished block, a title) is
/// raised on the parser thread, but the host posts it to the loop before any observer hears of it
/// (@see SessionHost's SessionSignal): neither the subscriptions nor the waiters are ever touched off the
/// loop, and a terminal is read only under its lock.
class ControlService final:
    public vtcontrol::ControlApi,
    public SessionStreamEvents,
    public vtworkspace::ModelEvents
{
  public:
    /// @param host The host whose windows, tabs and panes are served. Borrowed: it must outlive the service.
    /// @param config How the service is set up.
    ControlService(SessionHost& host, ControlServiceConfig config);

    ControlService(ControlService const&) = delete;
    ControlService(ControlService&&) = delete;
    ControlService& operator=(ControlService const&) = delete;
    ControlService& operator=(ControlService&&) = delete;
    ~ControlService() override = default;

    // clang-format off
    void handle(vtcontrol::CallContext& context, vtcontrol::DaemonInfoRequest const& request, vtcontrol::Completion<vtcontrol::DaemonInfoReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::ListRequest const& request, vtcontrol::Completion<vtcontrol::ListReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::CreateTabRequest const& request, vtcontrol::Completion<vtcontrol::CreateTabReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::SplitRequest const& request, vtcontrol::Completion<vtcontrol::SplitReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::SendRequest const& request, vtcontrol::Completion<vtcontrol::SendReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::CaptureRequest const& request, vtcontrol::Completion<vtcontrol::CaptureReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::ClosePaneRequest const& request, vtcontrol::Completion<vtcontrol::EmptyReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::CloseTabRequest const& request, vtcontrol::Completion<vtcontrol::EmptyReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::SetLabelRequest const& request, vtcontrol::Completion<vtcontrol::EmptyReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::BlockListRequest const& request, vtcontrol::Completion<vtcontrol::BlockListReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::BlockReadRequest const& request, vtcontrol::Completion<vtcontrol::BlockReadReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::WaitRequest const& request, vtcontrol::Completion<vtcontrol::WaitReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::SubscribeRequest const& request, vtcontrol::Completion<vtcontrol::SubscribeReply> done) override;
    void handle(vtcontrol::CallContext& context, vtcontrol::UnsubscribeRequest const& request, vtcontrol::Completion<vtcontrol::EmptyReply> done) override;
    // clang-format on
    using vtcontrol::ControlApi::handle;

    void clientAttached(std::string_view client) override;
    void clientDetached(std::string_view client) override;
    void sinkClosed(vtcontrol::EventSink& sink) override;

    // SessionStreamEvents: what the sessions raise.
    /// @return False: the service shows no screen, so it keeps no indicator row repainted.
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    void sessionBell(vtworkspace::SessionId session) override;
    void sessionClosing(vtworkspace::SessionId session) override;
    void sessionClosed(vtworkspace::SessionId session) override;
    void sessionSignal(vtworkspace::SessionId session, SessionSignal const& signal) override;

    // vtworkspace::ModelEvents: what the layout does.
    void tabAdded(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void tabClosed(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void paneSplit(vtworkspace::TabId tab,
                   vtworkspace::PaneId splitNode,
                   vtworkspace::PaneId newLeaf) override;
    void tabMoved(vtworkspace::WindowId /*window*/,
                  vtworkspace::TabId /*tab*/,
                  int /*from*/,
                  int /*to*/) override
    {
    }
    void activeTabChanged(vtworkspace::WindowId /*window*/,
                          vtworkspace::TabId /*tab*/,
                          int /*index*/) override
    {
    }
    void paneClosed(vtworkspace::TabId /*tab*/,
                    vtworkspace::PaneId /*closed*/,
                    vtworkspace::PaneId /*survivor*/) override
    {
    }
    void activePaneChanged(vtworkspace::TabId /*tab*/, vtworkspace::PaneId /*leaf*/) override {}
    void paneRatioChanged(vtworkspace::TabId /*tab*/,
                          vtworkspace::PaneId /*splitNode*/,
                          double /*ratio*/) override
    {
    }
    void tabTitleChanged(vtworkspace::TabId /*tab*/) override {}
    void tabColorChanged(vtworkspace::TabId /*tab*/) override {}

  private:
    /// A pane a target resolved to: its session and its UUID.
    struct PaneTarget
    {
        vtworkspace::SessionId session;
        vtcontrol::Uuid uuid;
    };

    /// @param session A pane's session.
    /// @param uuid Its UUID.
    /// @param what How the call named it, for the error.
    /// @return The pane, or `NotFound` when its terminal is gone.
    [[nodiscard]] std::expected<PaneTarget, vtcontrol::ControlError> livePane(vtworkspace::SessionId session,
                                                                              vtcontrol::Uuid uuid,
                                                                              std::string_view what) const;

    /// @return Every live pane, as target candidates.
    [[nodiscard]] std::vector<vtcontrol::TargetCandidate> paneCandidates() const;

    /// @param positioned The window whose tabs are addressable by position.
    /// @return Every tab, as target candidates.
    [[nodiscard]] std::vector<vtcontrol::TargetCandidate> tabCandidates(
        vtworkspace::WindowId positioned) const;

    /// @return Every window, as target candidates.
    [[nodiscard]] std::vector<vtcontrol::TargetCandidate> windowCandidates() const;

    /// The pane a call is about: @p spec, else the caller's pane, else the active pane of the active tab of
    /// the host's window.
    /// @param context The call.
    /// @param spec What the request named; empty for the default.
    /// @return The pane, or `NotFound` naming what was asked for.
    [[nodiscard]] std::expected<PaneTarget, vtcontrol::ControlError> resolvePane(
        vtcontrol::CallContext const& context, std::string_view spec) const;

    /// The tab a call is about: @p spec, else the caller pane's tab.
    [[nodiscard]] std::expected<vtworkspace::TabId, vtcontrol::ControlError> resolveTab(
        vtcontrol::CallContext const& context, std::string_view spec) const;

    /// The window a call is about: @p spec, else the caller pane's window, else the host's.
    [[nodiscard]] std::expected<vtworkspace::WindowId, vtcontrol::ControlError> resolveWindow(
        vtcontrol::CallContext const& context, std::string_view spec) const;

    /// A block a target resolved to: its pane, and its serial; none for the pane's last finished one.
    struct BlockTarget
    {
        PaneTarget pane;
        std::optional<uint64_t> serial;
    };

    /// @param context The call.
    /// @param block A block id; empty for the last finished block of @p pane.
    /// @param pane The pane, when @p block is empty.
    /// @return The block's pane and serial, or `NotFound` naming the block.
    [[nodiscard]] std::expected<BlockTarget, vtcontrol::ControlError> resolveBlock(
        vtcontrol::CallContext const& context, std::string_view block, std::string_view pane) const;

    /// @param context The call.
    /// @param object `<kind>:<spec>`, e.g. `pane:logs` or a full id.
    /// @return The object, `BadRequest` for an unknown kind, or `NotFound`.
    [[nodiscard]] std::expected<vtcontrol::ObjectRef, vtcontrol::ControlError> resolveObject(
        vtcontrol::CallContext const& context, std::string_view object) const;

    /// `tab.create`.
    [[nodiscard]] std::expected<vtcontrol::CreateTabReply, vtcontrol::ControlError> createTab(
        vtcontrol::CallContext const& context, vtcontrol::CreateTabRequest const& request);

    /// `pane.split`.
    [[nodiscard]] std::expected<vtcontrol::SplitReply, vtcontrol::ControlError> split(
        vtcontrol::CallContext const& context, vtcontrol::SplitRequest const& request);

    /// Ends @p session and closes its pane: the one place the API does, so that the request path that records
    /// why a session ended replaces one line.
    /// @param session The session.
    void closePane(vtworkspace::SessionId session);

    /// `workspace.list`.
    [[nodiscard]] std::expected<vtcontrol::ListReply, vtcontrol::ControlError> list(
        vtcontrol::CallContext const& context, vtcontrol::ListRequest const& request) const;

    /// `pane.capture` of a resolved pane.
    [[nodiscard]] vtcontrol::CaptureReply capture(PaneTarget const& pane, int32_t scrollback) const;

    /// `block.list` of a resolved pane.
    [[nodiscard]] vtcontrol::BlockListReply listBlocks(PaneTarget const& pane, uint32_t limit) const;

    /// `block.read` of a resolved block.
    [[nodiscard]] std::expected<vtcontrol::BlockReadReply, vtcontrol::ControlError> readBlock(
        BlockTarget const& block, vtcontrol::BlockPart part) const;

    /// @return The API record of one window, as the model has it now.
    [[nodiscard]] vtcontrol::WindowInfo windowInfoOf(vtworkspace::Window const& window,
                                                     uint32_t position) const;

    /// @param session The pane's session.
    /// @param tab The tab it is a leaf of, which says whether it is the active one.
    /// @return The API record of one pane, read under its terminal's lock.
    [[nodiscard]] vtcontrol::PaneInfo paneInfoOf(vtworkspace::SessionId session,
                                                 vtworkspace::Tab const& tab) const;

    /// @return The ids of @p session's pane, tab and window, as far as they still exist.
    [[nodiscard]] vtcontrol::EventOrigin originOf(vtworkspace::SessionId session) const;

    /// @return The ids of @p tab and its window.
    [[nodiscard]] vtcontrol::EventOrigin originOf(vtworkspace::WindowId window, vtworkspace::TabId tab) const;

    /// @param session The pane's session.
    /// @param block A block of it.
    /// @return The block's API record, read under the terminal's lock; nullopt when it is no longer kept.
    [[nodiscard]] std::optional<vtcontrol::BlockInfo> blockInfoFor(vtworkspace::SessionId session,
                                                                   vtbackend::CommandBlockId block) const;

    /// `block.wait`.
    void waitForBlock(vtcontrol::CallContext& context,
                      vtcontrol::WaitRequest const& request,
                      vtcontrol::Completion<vtcontrol::WaitReply> done);

    /// `pane.send` with `wait: commandFinished`: types the text, then waits for the block it runs in.
    void sendAndWait(vtcontrol::CallContext& context,
                     PaneTarget const& pane,
                     std::string_view text,
                     vtcontrol::Completion<vtcontrol::SendReply> done);

    SessionHost& _host;
    ControlServiceConfig _config;
    std::chrono::system_clock::time_point _startedAt; ///< When the service started: the daemon's uptime.
    EventHub _hub;                                    ///< Who takes which events.
    BlockWaiters _waiters;                            ///< Who waits for which block.
    /// A pane whose session is closing, and where it was before the model let go of it.
    struct ClosingPane
    {
        vtworkspace::SessionId session;
        vtcontrol::EventOrigin origin;
    };
    /// Set by sessionClosing(), taken by the sessionClosed() that follows it: `pane.closed` names its tab.
    std::optional<ClosingPane> _closing;
    /// Last, so that they end first: no event reaches a service being destroyed.
    ScopedStreamSubscription _streamSubscription;
    ScopedModelSubscription _modelSubscription;
};

} // namespace vthost::control
