// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistoryStore.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DefaultsEdition.hpp>
#include <contour/config/LayoutStore.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/platform/SpeechSynthesizer.hpp>
#include <contour/platform/TaskbarProgressSink.hpp>
#include <contour/session/CommandOutputFiles.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/InMemoryGuiStateStore.hpp>
#include <contour/test/LauncherFixtures.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/test/RecordingShellRcEditor.hpp>
#include <contour/window/DragEndProbe.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/WindowController.hpp>
#include <contour/window/WindowPositionStore.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <vtpty/ChannelPty.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <core/Environment.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour
{
class ContourGuiApp;
} // namespace contour

namespace contour::test
{

/// The notifier every session a test makes raises through, unless the test wants another.
///
/// It raises nothing. The platform's own on Linux is a FreeDesktopNotifier over the machine's session
/// bus, and no test may reach that bus: test_main.cpp fails a run that does. Pass it as a session's
/// notifier wherever a test constructs a TerminalSession itself; the factories below hand it out.
/// @return The notifier.
[[nodiscard]] inline std::unique_ptr<contour::platform::Notifier> makeTestNotifier()
{
    return std::make_unique<contour::platform::NullNotifier>();
}

/// A drag-end probe a test drives (@see window::DragEndProbe): it answers what the test set, and counts
/// what it was told.
class ScriptedDragEndProbe final: public contour::window::DragEndProbe
{
  public:
    void noteDragOver(QWindow const& /*window*/, QPointF /*local*/) override { ++sightings; }
    void noteDragLeft(QWindow const& /*window*/) override { ++exits; }
    [[nodiscard]] contour::window::PointerOverContour pointerOverContourAtEnd() const override
    {
        return over;
    }
    [[nodiscard]] contour::window::DropSpot dropSpot() const override { return spot; }
    void forgetDrag() override { ++forgotten; }

    /// What pointerOverContourAtEnd() answers: over no window, so a drag no window took tears off.
    contour::window::PointerOverContour over = contour::window::PointerOverContour::No;
    /// What dropSpot() answers.
    contour::window::DropSpot spot {};
    int sightings = 0; ///< How often noteDragOver() was called.
    int exits = 0;     ///< How often noteDragLeft() was called.
    int forgotten = 0; ///< How often forgetDrag() was called.
};

/// In-memory SessionFactory: every created session is backed by a vtpty::MockPty, so the manager's
/// session-creation paths (new tab, new split pane) run headlessly — no process is spawned, and a
/// test can seed/inspect each PTY's buffers. Records the cwd handed to each creation so
/// working-directory inheritance is assertable.
class MockPtySessionFactory final: public contour::session::SessionFactory
{
  public:
    /// The page size a created PTY falls back to when the caller passes no explicit @c pageSize (a
    /// brand-new window). Mirrors the profile default a real AppSessionFactory would use.
    static constexpr auto DefaultPageSize =
        vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) };

    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override
    {
        requestedCwds.push_back(std::move(cwd));
        boundForReconnect = false;
        // Record the size the manager asked for (nullopt == "use the profile default"), so a test can
        // assert that a new tab/split inherited the running window size instead of the default.
        requestedPageSizes.push_back(pageSize);
        requestedCommandOverrides.push_back(std::move(commandOverride));
        requestedProfileNames.push_back(std::move(profileName));
        auto const initialSize = pageSize.value_or(DefaultPageSize);
        if (makePty)
            return makePty(initialSize);
        auto pty = std::make_unique<vtpty::MockPty>(initialSize);
        createdPtys.push_back(pty.get());
        return pty;
    }

    [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
    {
        return reportedHosting;
    }

    [[nodiscard]] contour::session::TabMergeSupport tabMergeSupport() const noexcept override
    {
        return reportedTabMergeSupport.value_or(SessionFactory::tabMergeSupport());
    }

    [[nodiscard]] contour::session::RemoteRouting requestRemoteTabMerge(
        std::span<vtpty::Pty const* const> tabPtys,
        vtpty::Pty const* targetPty,
        vtworkspace::SplitSide side) override
    {
        requestedTabMerges.push_back(TabMergeRequest {
            .tabPtys = { tabPtys.begin(), tabPtys.end() }, .targetPty = targetPty, .side = side });
        if (reportedTabMergeRouting)
            return *reportedTabMergeRouting;
        return reportedHosting == contour::session::SessionHosting::Remote
                       && tabMergeSupport() == contour::session::TabMergeSupport::Supported
                   ? contour::session::RemoteRouting::Routed
                   : contour::session::RemoteRouting::Local;
    }

    [[nodiscard]] std::unique_ptr<contour::platform::Notifier> createNotifier() override
    {
        return makeNotifier ? makeNotifier() : nullptr;
    }

    [[nodiscard]] contour::session::SessionHostKind hostKind() const noexcept override
    {
        return reportedHostKind.value_or(SessionFactory::hostKind());
    }

    [[nodiscard]] contour::session::MoveSupport moveSupport() const noexcept override
    {
        return reportedMoveSupport.value_or(SessionFactory::moveSupport());
    }

    [[nodiscard]] std::optional<uint64_t> daemonInstance() const noexcept override
    {
        return reportedDaemonInstance;
    }

    /// The pty's index among createdPtys, plus hostSessionIdBase, under Remote hosting: a daemon's id for it.
    [[nodiscard]] std::optional<uint64_t> hostSessionIdOf(vtpty::Pty const& pty) const noexcept override
    {
        if (reportedHosting != contour::session::SessionHosting::Remote)
            return std::nullopt;
        auto const found = std::ranges::find(createdPtys, &pty);
        if (found == createdPtys.end())
            return std::nullopt;
        return hostSessionIdBase + static_cast<uint64_t>(std::distance(createdPtys.begin(), found));
    }

    /// Records the request; answers moveRouting, or Routed under Remote hosting and Local otherwise.
    [[nodiscard]] contour::session::RemoteRouting requestRemoteMove(
        contour::session::RemoteMove const& move) override
    {
        requestedMoves.push_back(move);
        if (moveRouting)
            return *moveRouting;
        return reportedHosting == contour::session::SessionHosting::Remote
                   ? contour::session::RemoteRouting::Routed
                   : contour::session::RemoteRouting::Local;
    }

    /// What hostKind() answers; the base's (Daemon under Remote hosting) when empty.
    std::optional<contour::session::SessionHostKind> reportedHostKind;
    /// What moveSupport() answers; the base's (Supported iff Local) when empty.
    std::optional<contour::session::MoveSupport> reportedMoveSupport;
    /// What daemonInstance() answers.
    std::optional<uint64_t> reportedDaemonInstance;
    /// The daemon id hostSessionIdOf() gives the first created pty.
    uint64_t hostSessionIdBase = 100;
    /// What requestRemoteMove() answers; see there when empty.
    std::optional<contour::session::RemoteRouting> moveRouting;
    /// Every requestRemoteMove() call, in order.
    std::vector<contour::session::RemoteMove> requestedMoves;

    [[nodiscard]] contour::session::HostedOperationRoute requestHostedOperation(
        vtpty::Pty const* /*pty*/, contour::session::HostedOperationRequest const& request) override
    {
        requestedHostedOperations.push_back(request);
        return hostedOperationRoute;
    }

    /// Records the request; routes it to the host when remoteRouting says so, as an attach-mode factory does.
    [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* /*actingPty*/,
                                        contour::session::RemoteSpawn const& spawn) override
    {
        requestedRemoteTabs.push_back(spawn);
        return remoteTabs == RemoteTabRouting::ToHost;
    }

    /// Records the request; routes it to the host when remoteTabs says so.
    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* /*actingPty*/,
                                          bool /*vertical*/,
                                          contour::session::RemoteSpawn const& /*spawn*/) override
    {
        ++requestedRemoteSplits;
        return remoteTabs == RemoteTabRouting::ToHost;
    }

    /// Records the call, as a host would be told a session was ended for good.
    void requestRemoteClose(vtpty::Pty const* /*pty*/) override { ++requestedRemoteCloses; }

    /// The factory of sessions that run here whatever the host. Under Remote hosting it is a view of this
    /// mock that allows creation, hosts locally and routes nothing, as the app's own factory does behind
    /// an attached host; else it is this mock.
    [[nodiscard]] contour::session::SessionFactory& localSessions() noexcept override
    {
        return reportedHosting == contour::session::SessionHosting::Remote
                   ? static_cast<contour::session::SessionFactory&>(_localView)
                   : *this;
    }

    /// Whether requestRemoteTab() and requestRemoteSplit() hand the request to the host.
    enum class RemoteTabRouting : uint8_t
    {
        Local = 0, ///< No: the manager builds the tab or split itself.
        ToHost,    ///< Yes: an attach-mode factory authoring it on its daemon.
    };
    RemoteTabRouting remoteTabs = RemoteTabRouting::Local;
    /// Every requestRemoteTab() call, in order.
    std::vector<contour::session::RemoteSpawn> requestedRemoteTabs;
    /// How often requestRemoteSplit() was asked.
    int requestedRemoteSplits = 0;
    /// How often requestRemoteClose() was called.
    int requestedRemoteCloses = 0;
    /// How many ptys localSessions() made, a subset of createdPtys.
    int localCreations = 0;

    [[nodiscard]] contour::session::ReconnectSupport reconnectSupport() const noexcept override
    {
        return reportedReconnectSupport;
    }

    [[nodiscard]] std::expected<void, contour::session::ReconnectError> reconnect() override
    {
        ++reconnects;
        return reconnectResult;
    }

    [[nodiscard]] contour::session::ReconnectBinding bindForReconnect(vtpty::Pty const* endedPty) override
    {
        if (!std::ranges::contains(namedForReconnect, endedPty))
            return contour::session::ReconnectBinding::Gone;
        boundForReconnect = true;
        return contour::session::ReconnectBinding::Bound;
    }

    void cancelReconnectBinding() override { boundForReconnect = false; }

    [[nodiscard]] bool canCreateSession() const noexcept override
    {
        return creation == SessionCreation::Allowed;
    }

    /// Whether canCreateSession() answers yes.
    enum class SessionCreation : uint8_t
    {
        Refused = 0,
        Allowed,
    };
    SessionCreation creation = SessionCreation::Allowed;
    /// Whether a bindForReconnect() is pending: createPty() consumes it, cancelReconnectBinding() drops it.
    bool boundForReconnect = false;

    /// What reconnectSupport() reports: Supported stands in for an app attached to a daemon.
    contour::session::ReconnectSupport reportedReconnectSupport =
        contour::session::ReconnectSupport::Unsupported;
    /// What reconnect() answers: success starts the supervised run a real controller would.
    std::expected<void, contour::session::ReconnectError> reconnectResult {};
    /// How often reconnect() was asked.
    int reconnects = 0;
    /// The ptys of the ended panes the returned layout still names: bindForReconnect() binds exactly these.
    std::vector<vtpty::Pty const*> namedForReconnect;

    /// What tabMergeSupport() reports instead of what the hosting implies: Supported stands in for a daemon
    /// that serves the merge, Unsupported for one that does not.
    std::optional<contour::session::TabMergeSupport> reportedTabMergeSupport = std::nullopt;
    /// What requestRemoteTabMerge() answers instead of what the hosting and support imply: Refused stands in
    /// for a daemon that cannot take the merge now.
    std::optional<contour::session::RemoteRouting> reportedTabMergeRouting = std::nullopt;
    /// One requestRemoteTabMerge() call.
    struct TabMergeRequest
    {
        std::vector<vtpty::Pty const*> tabPtys = {};
        vtpty::Pty const* targetPty = nullptr;
        vtworkspace::SplitSide side = vtworkspace::SplitSide::Left;
    };
    /// Every request requestRemoteTabMerge() was asked about, in order.
    std::vector<TabMergeRequest> requestedTabMerges;
    /// What hosting() reports: Remote stands in for an attach-mode factory, which runs no command here.
    contour::session::SessionHosting reportedHosting = contour::session::SessionHosting::Local;
    /// How createNotifier() makes each session's notifier: makeTestNotifier() unless a test says
    /// otherwise. A test that wants the platform's own -- the window's tray, off Linux -- unsets it.
    std::function<std::unique_ptr<contour::platform::Notifier>()> makeNotifier = makeTestNotifier;
    /// What requestHostedOperation() answers: RunOnHost stands in for a daemon, Refused for a tmux pane
    /// that has no command for the operation.
    contour::session::HostedOperationRoute hostedOperationRoute =
        contour::session::HostedOperationRoute::RunHere;
    /// Every request requestHostedOperation() was asked about, in order.
    std::vector<contour::session::HostedOperationRequest> requestedHostedOperations;
    std::vector<std::optional<std::string>> requestedCwds;
    /// The @c pageSize argument of each createPty() call (nullopt when the caller requested the default).
    std::vector<std::optional<vtbackend::PageSize>> requestedPageSizes;
    /// The @c commandOverride argument of each createPty() call.
    std::vector<std::optional<vtpty::Process::ExecInfo>> requestedCommandOverrides;
    /// The @c profileName argument of each createPty() call.
    std::vector<std::optional<std::string>> requestedProfileNames;
    /// Non-owning observation pointers; valid while the owning session lives. Empty for the ptys makePty
    /// made.
    std::vector<vtpty::MockPty*> createdPtys;
    /// Makes each pty instead of a MockPty when set -- a ChannelPty, for a test that feeds a mirror's bytes.
    std::function<std::unique_ptr<vtpty::Pty>(vtbackend::PageSize)> makePty {};

  private:
    /// What localSessions() hands out under Remote hosting: a local factory over this mock's ptys.
    class LocalView final: public contour::session::SessionFactory
    {
      public:
        explicit LocalView(MockPtySessionFactory& owner): _owner { owner } {}

        [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
            std::optional<std::string> cwd,
            std::optional<vtbackend::PageSize> pageSize = std::nullopt,
            std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
            std::optional<std::string> profileName = std::nullopt) override
        {
            ++_owner.localCreations;
            return _owner.createPty(
                std::move(cwd), pageSize, std::move(commandOverride), std::move(profileName));
        }

        [[nodiscard]] std::unique_ptr<contour::platform::Notifier> createNotifier() override
        {
            return _owner.createNotifier();
        }

      private:
        MockPtySessionFactory& _owner;
    };

    LocalView _localView { *this };
};

/// A TerminalSession a test builds itself, outside any session factory: over @p pty, raising through
/// @p notifier, or makeTestNotifier() when it is given none -- never the platform's own.
/// @param manager     The manager it reports to.
/// @param pty         Its PTY (owned from here on).
/// @param app         The application it runs in.
/// @param profileName The profile it runs under; empty takes the app's default one.
/// @param notifier    The notifier to raise through; null takes makeTestNotifier().
/// @return The session; the caller owns it.
[[nodiscard]] inline std::unique_ptr<contour::session::TerminalSession> makeTestSession(
    contour::session::TerminalSessionManager* manager,
    std::unique_ptr<vtpty::Pty> pty,
    contour::ContourGuiApp& app,
    std::string profileName = {},
    std::unique_ptr<contour::platform::Notifier> notifier = nullptr)
{
    return std::make_unique<contour::session::TerminalSession>(manager,
                                                               std::move(pty),
                                                               app,
                                                               std::move(profileName),
                                                               std::nullopt,
                                                               std::nullopt,
                                                               notifier ? std::move(notifier)
                                                                        : makeTestNotifier());
}

/// A TerminalSession a test builds itself, decoding keys with the platforms @p keyboardPlatforms builds
/// rather than the build host's: what lets every platform's key path run on any machine.
/// @param manager           The manager it reports to.
/// @param pty               Its PTY (owned from here on).
/// @param app               The application it runs in.
/// @param keyboardPlatforms Builds the session's keyboard platform for its profile's policy.
/// @return The session; the caller owns it.
[[nodiscard]] inline std::unique_ptr<contour::session::TerminalSession> makeTestSession(
    contour::session::TerminalSessionManager* manager,
    std::unique_ptr<vtpty::Pty> pty,
    contour::ContourGuiApp& app,
    contour::input::KeyboardPlatformFactory keyboardPlatforms)
{
    return std::make_unique<contour::session::TerminalSession>(manager,
                                                               std::move(pty),
                                                               app,
                                                               std::string {},
                                                               std::nullopt,
                                                               std::nullopt,
                                                               makeTestNotifier(),
                                                               contour::session::SessionRole::Shell,
                                                               contour::session::SessionHosting::Local,
                                                               std::move(keyboardPlatforms));
}

/// A real session factory with every call passed through, except that a session it would hand the
/// platform's own notifier raises through makeTestNotifier() instead.
///
/// For the factories a test cannot otherwise give a notifier: the remote controllers the attach tests
/// drive are the real ones, and final. TestApp wraps every factory but the harness's own in one.
class TestNotifierSessionFactory final: public contour::session::SessionFactory
{
  public:
    /// @param factory The factory every call is passed to (owned).
    explicit TestNotifierSessionFactory(std::unique_ptr<contour::session::SessionFactory> factory):
        _factory { std::move(factory) }
    {
    }

    [[nodiscard]] std::unique_ptr<contour::platform::Notifier> createNotifier() override
    {
        if (auto notifier = _factory->createNotifier())
            return notifier;
        return makeTestNotifier();
    }

    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override
    {
        return _factory->createPty(
            std::move(cwd), pageSize, std::move(commandOverride), std::move(profileName));
    }

    [[nodiscard]] bool canCreateSession() const noexcept override { return _factory->canCreateSession(); }

    [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
    {
        return _factory->hosting();
    }

    [[nodiscard]] contour::session::SessionFactory& localSessions() noexcept override
    {
        auto& local = _factory->localSessions();
        return &local == _factory.get() ? *this : local;
    }

    [[nodiscard]] contour::session::TabMergeSupport tabMergeSupport() const noexcept override
    {
        return _factory->tabMergeSupport();
    }

    [[nodiscard]] contour::session::RemoteRouting requestRemoteTabMerge(
        std::span<vtpty::Pty const* const> tabPtys,
        vtpty::Pty const* targetPty,
        vtworkspace::SplitSide side) override
    {
        return _factory->requestRemoteTabMerge(tabPtys, targetPty, side);
    }

    [[nodiscard]] contour::session::MoveSupport moveSupport() const noexcept override
    {
        return _factory->moveSupport();
    }

    [[nodiscard]] contour::session::SessionHostKind hostKind() const noexcept override
    {
        return _factory->hostKind();
    }

    [[nodiscard]] std::optional<uint64_t> daemonInstance() const noexcept override
    {
        return _factory->daemonInstance();
    }

    [[nodiscard]] std::optional<uint64_t> hostSessionIdOf(vtpty::Pty const& pty) const noexcept override
    {
        return _factory->hostSessionIdOf(pty);
    }

    [[nodiscard]] contour::session::RemoteRouting requestRemoteMove(
        contour::session::RemoteMove const& move) override
    {
        return _factory->requestRemoteMove(move);
    }

    [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty,
                                        contour::session::RemoteSpawn const& spawn) override
    {
        return _factory->requestRemoteTab(actingPty, spawn);
    }

    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* actingPty,
                                          bool vertical,
                                          contour::session::RemoteSpawn const& spawn) override
    {
        return _factory->requestRemoteSplit(actingPty, vertical, spawn);
    }

    [[nodiscard]] contour::session::RemoteRouting requestRemoteSplitOrientation(
        vtpty::Pty const* anchorPty, vtworkspace::SplitState axis) override
    {
        return _factory->requestRemoteSplitOrientation(anchorPty, axis);
    }

    void reportSplitRatio(vtpty::Pty const* firstPty, vtpty::Pty const* secondPty, double ratio) override
    {
        _factory->reportSplitRatio(firstPty, secondPty, ratio);
    }

    void reportTabPresentation(contour::session::LocalTabPresentation const& presentation) override
    {
        _factory->reportTabPresentation(presentation);
    }

    [[nodiscard]] bool requestRemoteWindow(contour::session::RemoteSpawn const& spawn) override
    {
        return _factory->requestRemoteWindow(spawn);
    }

    void requestRemoteClose(vtpty::Pty const* pty) override { _factory->requestRemoteClose(pty); }

    [[nodiscard]] contour::session::HostedOperationRoute requestHostedOperation(
        vtpty::Pty const* pty, contour::session::HostedOperationRequest const& request) override
    {
        return _factory->requestHostedOperation(pty, request);
    }

    void bindTerminal(vtpty::Pty const* pty,
                      vtbackend::Terminal& terminal,
                      contour::session::InterruptionSink const& interruptions) override
    {
        _factory->bindTerminal(pty, terminal, interruptions);
    }

    [[nodiscard]] contour::session::ReconnectSupport reconnectSupport() const noexcept override
    {
        return _factory->reconnectSupport();
    }

    [[nodiscard]] std::expected<void, contour::session::ReconnectError> reconnect() override
    {
        return _factory->reconnect();
    }

    [[nodiscard]] contour::session::ReconnectBinding bindForReconnect(vtpty::Pty const* endedPty) override
    {
        return _factory->bindForReconnect(endedPty);
    }

    void cancelReconnectBinding() override { _factory->cancelReconnectBinding(); }

  private:
    std::unique_ptr<contour::session::SessionFactory> _factory;
};

/// @p factory as TestApp installs it: the harness's own MockPtySessionFactory decides each session's
/// notifier itself, and any other is wrapped in a TestNotifierSessionFactory.
/// @param factory The factory a test handed TestApp, or null for the application's own.
/// @return The factory to install.
[[nodiscard]] inline std::unique_ptr<contour::session::SessionFactory> withTestNotifier(
    std::unique_ptr<contour::session::SessionFactory> factory)
{
    if (factory == nullptr || dynamic_cast<MockPtySessionFactory const*>(factory.get()) != nullptr)
        return factory;
    return std::make_unique<TestNotifierSessionFactory>(std::move(factory));
}

/// A ProcessLiveness that can tell nothing about any process.
class UnknownProcessLiveness final: public contour::session::ProcessLiveness
{
  public:
    [[nodiscard]] contour::session::ProcessState stateOf(int64_t /*processId*/) const override
    {
        return contour::session::ProcessState::Unknown;
    }
};

/// The MockPty backing @p session, for seeding output and inspecting the bytes the terminal wrote
/// towards the shell (key/mouse encodings, replies, focus events).
/// @param session A session created over a MockPty (via MockPtySessionFactory or directly).
/// @return The session's PTY, downcast. Throws std::bad_cast if it is not a MockPty.
[[nodiscard]] inline vtpty::MockPty& mockPtyOf(contour::session::TerminalSession& session)
{
    return dynamic_cast<vtpty::MockPty&>(session.terminal().device());
}

/// An in-memory LayoutStore: SaveLayout runs end to end (serialize -> persist -> re-read) with no
/// filesystem at all, and a test can inspect exactly what was handed to persistence. @c loadError
/// makes the store report an unreadable backing file, to exercise the refuse-rather-than-destroy
/// path without corrupting a real one.
class InMemoryLayoutStore final: public contour::config::LayoutStore
{
  public:
    [[nodiscard]] std::expected<contour::config::LayoutMap, std::string> load(
        std::filesystem::path const& path) const override
    {
        loadedPaths.push_back(path);
        if (loadError)
            return std::unexpected(*loadError);
        return layouts;
    }

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        contour::config::LayoutMap const& newLayouts) override
    {
        savedPaths.push_back(path);
        if (saveError)
            return std::unexpected(*saveError);
        layouts = newLayouts;
        return {};
    }

    /// The store's contents (seed it to model a pre-existing layouts.yml; read it back to assert
    /// what SaveLayout persisted).
    contour::config::LayoutMap layouts;
    /// When set, load() fails with this message (an unreadable/corrupt backing file).
    std::optional<std::string> loadError;
    /// When set, save() fails with this message (permissions, disk full, ...).
    std::optional<std::string> saveError;
    /// The path each load()/save() was asked for, so a test can assert WHERE layouts persist.
    mutable std::vector<std::filesystem::path> loadedPaths;
    std::vector<std::filesystem::path> savedPaths;
};

/// A SpeechSynthesizer that records what it was asked to say instead of saying it.
///
/// The read-aloud path cannot otherwise be driven headlessly: the real synthesizer answers
/// available() only by connecting to the machine's speech service and enumerating its voices, so a
/// test would depend on the developer's installed voices and would speak out loud when they have one.
class RecordingSpeechSynthesizer final: public contour::platform::SpeechSynthesizer
{
  public:
    [[nodiscard]] bool available() const override { return isAvailable; }
    void say(std::string_view text) override { spoken.emplace_back(text); }
    void stop() override { ++stopCount; }

    /// Everything say() was handed, in order — already passed through speakableText().
    std::vector<std::string> spoken;
    /// How many times stop() was asked for.
    size_t stopCount = 0;
    /// What available() reports (flip to false for the "no voice on this machine" path).
    bool isAvailable = true;
};

// The former fixture-local BlockingMockPty (an in-memory PTY with real blocking-read semantics
// for tests running a LIVE session read loop) now ships as vtpty::ChannelPty, so production
// remote sessions run on the identical Pty.

/// PTY whose start() produces a caller-chosen outcome, so every way a device can come up — or fail
/// to — is drivable from one fixture rather than a family of near-identical ones.
///
/// The Throw case is the shape of the #1711 crash: on Windows a CreateProcess() failure is
/// discovered in the PARENT (there is no fork), and the only channel Process::start() had was an
/// exception. Thrown from TerminalSession::start(), it unwound out of TerminalDisplay::setSession()
/// mid-mutation — past emit sessionChanged() — leaving a half-bound display and a session that,
/// never having started its exit watcher, no onClosed() could ever prune. It is kept as a case
/// because no device may take the process down that way, whatever it throws.
///
/// Built on vtpty::ChannelPty rather than vtpty::MockPty because the Diagnose case must leave a
/// session that is genuinely UP: MockPty answers an empty buffer with a zero-length read, which
/// Terminal::processInputOnce() reads as EOF — so the read loop this start() has just launched would
/// close the device from under the test within microseconds of returning. That is a race a debug build
/// happens to win and a release build loses. ChannelPty blocks instead, which is why production
/// remote sessions run on it too.
class ConfigurableStartPty final: public vtpty::ChannelPty
{
  public:
    /// What start() does.
    enum class StartBehavior : uint8_t
    {
        Diagnose, ///< Starts, reporting what it had to give up (a spawn-ladder rung below the first).
        Fail,     ///< Reports a failure as a value, the way vtpty does.
        Throw,    ///< Throws, the way the Windows spawn used to. See issue #1711.
    };

    /// The message the Windows reporters in #1711 actually saw.
    static constexpr auto FailureText = std::string_view { "The directory name is invalid." };

    /// A non-fatal notice, as the spawn ladder's directory-dropping rung produces.
    static constexpr auto DiagnosticText =
        std::string_view { "Failed to start in \"/gone\". Using the current directory instead." };

    ConfigurableStartPty(vtbackend::PageSize pageSize, StartBehavior behavior):
        vtpty::ChannelPty { pageSize }, _behavior { behavior }
    {
    }

    vtpty::StartResult start() override
    {
        switch (_behavior)
        {
            case StartBehavior::Throw: throw std::runtime_error(std::string { FailureText });
            case StartBehavior::Fail:
                return std::unexpected(vtpty::StartFailure { .error = vtpty::StartError::SpawnFailed,
                                                             .detail = std::string { FailureText } });
            case StartBehavior::Diagnose:
                return vtpty::ChannelPty::start().transform([](vtpty::StartOutcome outcome) {
                    outcome.diagnostic = std::string { DiagnosticText };
                    return outcome;
                });
        }
        return {};
    }

  private:
    StartBehavior _behavior;
};

/// The process's own environment, but for the names a test sets, which read as the test set them.
class TestEnvironment final: public core::Environment
{
  public:
    /// The names a test sets, and their values.
    using Overrides = std::map<std::string, std::string, std::less<>>;

    /// @param overrides The names a test sets; every other name reads as the process's own environment has
    /// it.
    explicit TestEnvironment(Overrides overrides = {}): _overrides { std::move(overrides) } {}

    /// @param name Name of the variable to look up.
    /// @return What the test set it to; else what the process's own environment holds.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override
    {
        if (auto const i = _overrides.find(name); i != _overrides.end())
            return i->second;
        return _live.get(name);
    }

  private:
    core::LiveEnvironment _live;
    Overrides _overrides;
};

/// In-memory window location store; never reads or writes the test machine's user state.
class InMemoryWindowPositionStore final: public contour::window::WindowPositionStore
{
  public:
    std::optional<contour::window::WindowPosition> value;
    mutable size_t loads = 0;
    size_t saves = 0;
    [[nodiscard]] std::expected<std::optional<contour::window::WindowPosition>, std::string> load(
        std::filesystem::path const&) const override
    {
        ++loads;
        return value;
    }
    [[nodiscard]] std::expected<void, std::string> save(
        std::filesystem::path const&, contour::window::WindowPosition const& position) override
    {
        value = position;
        ++saves;
        return {};
    }
};

/// Records the manager's native taskbar output without reaching the OS shell.
class RecordingTaskbar final: public contour::platform::TaskbarProgressSink
{
  public:
    /// Whether this test adapter supports publishing progress.
    enum class Support : uint8_t
    {
        Unavailable, ///< Models a platform without a native adapter.
        Available,   ///< Records scheduled updates.
    };

    /// @param support Whether this adapter accepts scheduled updates.
    /// @param scope   Whether it models a per-window or a per-application indicator.
    explicit RecordingTaskbar(
        Support support = Support::Available,
        contour::platform::TaskbarProgressScope scope = contour::platform::TaskbarProgressScope::Window):
        _support(support), _scope(scope)
    {
    }
    [[nodiscard]] bool isSupported() const noexcept override { return _support == Support::Available; }
    [[nodiscard]] contour::platform::TaskbarProgressScope scope() const noexcept override { return _scope; }
    void setApplicationProgress(contour::platform::TaskbarProgress progress) override
    {
        application = progress;
    }
    void setApplicationAttention(contour::platform::TaskbarAttention attention) override
    {
        applicationAttention.push_back(attention);
    }
    void bindWindow(uint64_t, QWindow&) override {}
    void setProgress(uint64_t window, contour::platform::TaskbarProgress progress) override
    {
        windows[window] = progress;
    }
    void setAttention(uint64_t window, contour::platform::TaskbarAttention attention) override
    {
        windowAttention.emplace_back(window, attention);
    }
    void removeWindow(uint64_t window) override { windows.erase(window); }

    std::unordered_map<uint64_t, contour::platform::TaskbarProgress>
        windows;                                    ///< The last progress per window.
    contour::platform::TaskbarProgress application; ///< The last application progress.
    /// Every count the application indicator was told, in order.
    std::vector<contour::platform::TaskbarAttention> applicationAttention;
    /// Every count a window's button was told, in order, with the window.
    std::vector<std::pair<uint64_t, contour::platform::TaskbarAttention>> windowAttention;

  private:
    Support _support;
    contour::platform::TaskbarProgressScope _scope;
};

/// Owns one manager-minted WindowController for a test's lifetime. Production deletes controllers
/// through removeWindowController() (which deleteLater()s them); the guard replays that and drains
/// the deferred-delete queue so LeakSanitizer sees a clean teardown. Removal is idempotent, so a
/// test may also remove the controller explicitly.
struct ScopedController
{
    contour::session::TerminalSessionManager& manager;
    contour::window::WindowController* controller;
    vtworkspace::WindowId id;

    explicit ScopedController(contour::session::TerminalSessionManager& m):
        manager(m), controller(m.createWindowController()), id(controller->windowId())
    {
    }
    /// A controller whose menus open as @p menuPresentation says, whatever the app's style says.
    ScopedController(contour::session::TerminalSessionManager& m,
                     contour::config::MenuPresentation menuPresentation):
        manager(m), controller(m.createWindowController(menuPresentation)), id(controller->windowId())
    {
    }
    /// A controller whose notices are said through what @p noticeAnnouncer builds.
    ScopedController(contour::session::TerminalSessionManager& m,
                     contour::window::NoticeAnnouncerFactory const& noticeAnnouncer):
        manager(m),
        controller(m.createWindowController(contour::config::MenuPresentation::InScene, noticeAnnouncer)),
        id(controller->windowId())
    {
    }
    ~ScopedController()
    {
        manager.removeWindowController(id); // no-op if the test already removed it
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    ScopedController(ScopedController const&) = delete;
    ScopedController& operator=(ScopedController const&) = delete;
    ScopedController(ScopedController&&) = delete;
    ScopedController& operator=(ScopedController&&) = delete;

    [[nodiscard]] contour::window::WindowController* operator->() const noexcept { return controller; }
    [[nodiscard]] contour::window::WindowController& operator*() const noexcept { return *controller; }
};

/// The OS window half of @ref ScopedControllerWithOsWindow, a base of its own so that it is built
/// before the controller and destroyed after it.
struct OsWindowHolder
{
    std::unique_ptr<QQuickWindow> osWindow = std::make_unique<QQuickWindow>(); ///< Never shown.
};

/// A @ref ScopedController with an OS window it may be bound to (`bindWindow(osWindow.get())`), for a
/// test that needs the window's own state -- minimised, active. The controller keeps a raw pointer to
/// the window once bound, so the window outlives it: OsWindowHolder is the first base.
struct ScopedControllerWithOsWindow: OsWindowHolder, ScopedController
{
    explicit ScopedControllerWithOsWindow(contour::session::TerminalSessionManager& m): ScopedController(m) {}
};

/// Closes every tab of @p window, last first, so the sessions wind down as a user closing them would.
inline void closeAllTabs(contour::window::WindowController& window)
{
    for (auto const row: std::views::iota(0, window.count()) | std::views::reverse)
        window.closeTabAtIndex(row);
}

/// Tabs that each have a real (mock-PTY) session: their ids and session ids in row order.
struct SessionTabs
{
    std::vector<vtworkspace::TabId> tabs {};
    std::vector<vtworkspace::SessionId> sessions {};
};

/// Opens @p n tabs in @p controller's window, each with a session of its own.
/// @param manager    The session manager owning @p controller.
/// @param controller The window to open the tabs in.
/// @param n          How many tabs to open.
/// @return The tabs and their sessions, in row order.
[[nodiscard]] inline SessionTabs createSessionTabs(contour::session::TerminalSessionManager& manager,
                                                   contour::window::WindowController& controller,
                                                   int n)
{
    auto result = SessionTabs {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, n))
    {
        controller.createNewTab();
        auto const* tab = manager.model().window(controller.windowId())->activeTab();
        REQUIRE(tab != nullptr);
        REQUIRE(controller.activeSession() != nullptr);
        result.tabs.push_back(tab->id());
        result.sessions.push_back(controller.activeSession()->modelSessionId());
    }
    return result;
}

/// The node titled @p title in @p nodes, searching submenus too.
/// @param nodes The nodes to search.
/// @param title The title to find.
/// @return The node, or nullptr if there is none.
[[nodiscard]] inline contour::command::MenuNode const* nodeTitled(
    std::span<contour::command::MenuNode const> nodes, std::string_view title)
{
    for (auto const& node: nodes)
    {
        if (node.title == title)
            return &node;
        if (auto const* nested = nodeTitled(node.children, title))
            return nested;
    }
    return nullptr;
}

/// Whether an action ran to its end while another thread held a lock.
enum class UnderLock : uint8_t
{
    Waited = 0, ///< It could finish only once the lock was let go.
    Finished,   ///< It finished while the lock was held.
};

/// Runs @p action while another thread holds @p lockable -- as a parser holds its terminal across a whole
/// write -- and tells whether it finished without waiting for that thread.
///
/// The holder lets go as soon as @p action returns, or once @p bound has passed: a regression costs the
/// bound rather than hanging the suite, and a pass costs nothing.
/// @param lockable What the other thread holds.
/// @param action   What must not wait for it.
/// @param bound    The longest the other thread holds on while @p action has not returned.
/// @return Finished when @p action returned while the lock was still held.
template <typename Lockable, typename Action>
[[nodiscard]] UnderLock runWhileHeld(Lockable& lockable,
                                     Action&& action,
                                     std::chrono::milliseconds bound = std::chrono::milliseconds { 500 })
{
    auto mutex = std::mutex {};
    auto changed = std::condition_variable {};
    auto held = false;
    auto done = false;
    auto released = std::atomic<bool> { false };

    auto holder = std::thread { [&]() {
        auto const hold = std::scoped_lock { lockable };
        {
            auto lock = std::unique_lock { mutex };
            held = true;
            changed.notify_all();
            changed.wait_for(lock, bound, [&]() { return done; });
        }
        released.store(true, std::memory_order_release);
    } };

    {
        auto lock = std::unique_lock { mutex };
        changed.wait(lock, [&]() { return held; });
    }
    std::forward<Action>(action)();
    auto const outcome = released.load(std::memory_order_acquire) ? UnderLock::Waited : UnderLock::Finished;

    {
        auto const lock = std::scoped_lock { mutex };
        done = true;
    }
    changed.notify_all();
    holder.join();
    return outcome;
}

} // namespace contour::test
