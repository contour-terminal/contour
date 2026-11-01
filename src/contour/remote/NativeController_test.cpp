// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/HostTransport.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RoutingSessionFactory.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/RecordingAnnouncer.hpp>

#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>

#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <contour/remote/RemoteLayout.hpp>

#include <core/async/Cancellation.hpp>
#include <core/log/LogSink.hpp>
#include <core/log/LogStore.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshot.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;

namespace
{

/// The timeline a test controller stamps, on a clock nothing advances.
struct TestTimeline
{
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} };
    vthost::latency::StartupTimeline timeline { clock, clock.now() };
};

/// Builds a controller as every test here does: no attach settings of its own, on the steady clock.
/// The one place the constructor is spelled, so a collaborator it gains is added here alone.
/// @param endpoint The daemon to attach to, over TLS.
/// @param timeline What the controller stamps; it must outlive the controller.
/// @param sessionSettings Asked before each session the controller creates; empty to keep none.
/// @param colorPreference The light/dark preference its first hello states; none states none.
/// @return The controller, not yet connecting.
[[nodiscard]] std::unique_ptr<contour::remote::NativeController> makeNativeController(
    vthost::TcpEndpoint endpoint,
    TestTimeline& timeline,
    contour::remote::NativeController::SessionSettingsSource sessionSettings = {},
    std::optional<vtbackend::ColorPreference> colorPreference = std::nullopt)
{
    return std::make_unique<contour::remote::NativeController>(
        std::make_shared<vthost::TlsTransportFactory>(std::move(endpoint)),
        vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows, .windows = {} },
        std::string {},
        std::nullopt,
        std::move(sessionSettings),
        colorPreference,
        core::platform::defaultSteadyClock(),
        timeline.timeline);
}

/// Captures every log category into one buffer that several threads may write while another
/// reads it. core::log::ScopedCapture is single-threaded by design; a test with a daemon thread and
/// a GUI reactor thread logging while the test thread polls would race on its buffer, which
/// corrupts the heap.
class SharedLogCapture
{
  public:
    SharedLogCapture()
    {
        for (auto const& each: core::log::get())
        {
            auto& target = each.get();
            _restorePoints.push_back(RestorePoint {
                .target = &target, .previousSink = &target.sink(), .wasEnabled = target.isEnabled() });
            target.enable();
            target.setSink(_sink);
        }
    }

    /// Restores every category. Runs after the threads that logged have been joined: declare the
    /// capture before the objects that own them.
    ~SharedLogCapture()
    {
        for (auto const& point: _restorePoints)
        {
            point.target->setSink(*point.previousSink);
            point.target->enable(point.wasEnabled);
        }
    }

    SharedLogCapture(SharedLogCapture const&) = delete;
    SharedLogCapture& operator=(SharedLogCapture const&) = delete;
    SharedLogCapture(SharedLogCapture&&) = delete;
    SharedLogCapture& operator=(SharedLogCapture&&) = delete;

    /// @param needle The substring to look for.
    /// @return Whether any captured output contains @p needle.
    [[nodiscard]] bool contains(std::string_view needle) const
    {
        auto const lock = std::lock_guard { _mutex };
        return _text.contains(needle);
    }

    /// @return A copy of everything captured so far.
    [[nodiscard]] std::string text() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _text;
    }

  private:
    struct RestorePoint
    {
        core::log::Category* target;
        core::log::Sink* previousSink;
        bool wasEnabled;
    };

    mutable std::mutex _mutex;
    std::string _text;
    core::log::Sink _sink { true, [this](std::string_view const& text) {
                               auto const lock = std::lock_guard { _mutex };
                               _text += text;
                           } };
    std::vector<RestorePoint> _restorePoints;
};

/// An in-process `contour daemon` (native protocol) on its own thread, serving
/// TLS over a LOOPBACK TCP socket on an EPHEMERAL port. Using TCP (the same
/// transport `client --connect-tcp` uses) rather than AF_UNIX lets these
/// end-to-end tests run on every platform, Windows included.
/// A daemon end of a connection that the fixture can drop, as a crashing daemon would: it is listed in
/// @c live for as long as it exists.
class DroppableSocket final: public core::net::testing::SocketDecorator
{
  public:
    DroppableSocket(std::unique_ptr<core::net::ISocket> inner, std::set<DroppableSocket*>& live):
        SocketDecorator(*inner), _inner(std::move(inner)), _live(live)
    {
        _live.insert(this);
    }
    ~DroppableSocket() override { _live.erase(this); }

    DroppableSocket(DroppableSocket const&) = delete;
    DroppableSocket& operator=(DroppableSocket const&) = delete;
    DroppableSocket(DroppableSocket&&) = delete;
    DroppableSocket& operator=(DroppableSocket&&) = delete;

  private:
    std::unique_ptr<core::net::ISocket> _inner;
    std::set<DroppableSocket*>& _live;
};

/// A daemon end of a connection that can hold back what the daemon writes once the LayoutState of one window
/// has gone out, until released: a test then has the client receive one window's layout of a push and not yet
/// the next, as two frames the GUI may reconcile between. Daemon thread only. Writes pass straight through
/// while nothing is held.
class GatedSocket final: public core::net::testing::SocketDecorator
{
  public:
    using Written = std::expected<std::size_t, core::net::NetError>;

    GatedSocket(std::unique_ptr<core::net::ISocket> inner, std::set<GatedSocket*>& live):
        SocketDecorator(*inner), _inner(std::move(inner)), _live(live)
    {
        _live.insert(this);
    }
    ~GatedSocket() override { _live.erase(this); }

    GatedSocket(GatedSocket const&) = delete;
    GatedSocket& operator=(GatedSocket const&) = delete;
    GatedSocket(GatedSocket&&) = delete;
    GatedSocket& operator=(GatedSocket&&) = delete;

    /// Lets every frame through up to and including the next LayoutState of @p window, and holds the rest.
    void holdAfterLayoutOf(uint64_t window) { _holdAfter = window; }

    /// Lets every frame through up to the next LayoutState, and holds it and the rest: the connection is up,
    /// and no layout of it has come in.
    void holdAtNextLayout() { _holdAtLayout = true; }

    /// Sends what was held, in order, and holds nothing more.
    void release(core::net::EventLoop& loop)
    {
        _holdAfter.reset();
        _holdAtLayout = false;
        _holding = false;
        _flushing = true;
        loop.spawn(flush());
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        if (passesStraight())
            return _inner->write(buffer);
        return core::net::IoAwaitable { take({ buffer.begin(), buffer.end() }) };
    }

    [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                       std::shared_ptr<void const> keepAlive = {}) override
    {
        if (passesStraight())
            return _inner->writeVectored(segments, std::move(keepAlive));
        auto bytes = std::vector<std::byte> {};
        for (auto const segment: segments)
            bytes.insert(bytes.end(), segment.begin(), segment.end());
        return core::net::IoAwaitable { take(std::move(bytes)) };
    }

  private:
    [[nodiscard]] bool passesStraight() const noexcept { return !armed() && !_flushing && _stream.empty(); }

    [[nodiscard]] bool armed() const noexcept { return _holdAfter || _holdAtLayout; }

    /// Takes one write: what may pass is written on, the rest kept.
    [[nodiscard]] core::async::Task<Written> take(std::vector<std::byte> bytes)
    {
        auto const total = bytes.size();
        _stream.insert(_stream.end(), bytes.begin(), bytes.end());
        if (_holding || _flushing)
            co_return total;
        // Whole frames only, while armed: the one that trips the gate ends what passes.
        auto passing = armed() ? std::size_t { 0 } : _stream.size();
        while (armed() && !_holding)
        {
            auto const decoded =
                vthost::proto::decodePdu(std::span<std::byte const> { _stream }.subspan(passing));
            if (!decoded)
                break;
            auto const* layout = std::get_if<vthost::proto::LayoutState>(&decoded->pdu);
            if (layout != nullptr && _holdAtLayout)
            {
                _holding = true;
                break;
            }
            passing += decoded->consumed;
            if (layout != nullptr && layout->window == *_holdAfter)
                _holding = true;
        }
        auto chunk = std::vector<std::byte>(_stream.begin(), _stream.begin() + static_cast<long>(passing));
        _stream.erase(_stream.begin(), _stream.begin() + static_cast<long>(passing));
        if (auto const written = co_await writeAll(std::move(chunk)); !written)
            co_return std::unexpected { written.error() };
        co_return total;
    }

    [[nodiscard]] core::async::Task<Written> writeAll(std::vector<std::byte> chunk)
    {
        auto done = std::size_t { 0 };
        while (done < chunk.size())
        {
            auto const written = co_await _inner->write(std::span<std::byte const> { chunk }.subspan(done));
            if (!written)
                co_return std::unexpected { written.error() };
            done += *written;
        }
        co_return done;
    }

    [[nodiscard]] core::async::Task<void> flush()
    {
        while (!_stream.empty())
            std::ignore = co_await writeAll(std::exchange(_stream, {}));
        _flushing = false;
    }

    std::unique_ptr<core::net::ISocket> _inner;
    std::set<GatedSocket*>& _live;
    std::optional<uint64_t> _holdAfter;
    bool _holdAtLayout = false; ///< Holds from the next LayoutState on, that one included.
    bool _holding = false;      ///< The gate tripped: everything since is kept.
    bool _flushing = false;     ///< What was kept is being written on; what comes meanwhile queues behind it.
    std::vector<std::byte> _stream;
};

struct DaemonFixture
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::shared_ptr<vthost::IExtensionHosting> extensions;
    vthost::NativeSessionServices services;
    std::unique_ptr<vthost::SessionHost> host;
    /// Serves each accepted connection on @c host; replaced with it by restartHost().
    vthost::ConnectionHandler native;
    std::unique_ptr<vthost::ConnectionAcceptor> server;
    std::set<DroppableSocket*> live; ///< The daemon ends of the connections up now. Daemon thread only.
    std::set<GatedSocket*> gated;    ///< The same ends, as the gates in front of them. Daemon thread only.
    /// Whether each connection accepted from now on holds its first layout and what follows, until
    /// releaseHeld(). Daemon thread only.
    bool holdNewAtLayout = false;
    std::uint16_t port = 0; ///< The OS-assigned loopback port the daemon listens on.
    std::thread thread;
    bool cancelled = false; ///< Whether teardown unwound the accept loop.
    uint64_t instance = 1;  ///< Which run of the daemon @c host is; 0 states none.
    /// The host's profiles, by name; none resolves every spawn to the host's own settings.
    std::shared_ptr<vthost::ProfileCatalog const> profiles;
    /// The workspace the first host restores as it starts; none restores nothing.
    std::optional<vthost::WorkspaceRestore> restore;
    /// The tabs the host opens on construction, as `default_layout` would; none opens none.
    vtworkspace::Layout startupLayout;

    /// @return A host of the fixture's run @c instance, serving MockPty sessions.
    [[nodiscard]] std::unique_ptr<vthost::SessionHost> makeHost()
    {
        return std::make_unique<vthost::SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            vtbackend::Settings {},
            core::defaultEnvironment(),
            vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                        .startupLayout = startupLayout,
                                        .extensions = extensions,
                                        .profiles = profiles,
                                        .instance = instance,
                                        .restore = std::exchange(restore, std::nullopt) });
    }

    /// @param extensions The status-line extensions the hosted sessions run; none by default.
    /// @param services What each native connection is served with: what its handshake admits.
    /// @param instance Which run of the daemon the first host is; 0 for one that states none.
    /// @param profiles The host's profiles; none by default.
    /// @param restore The workspace the first host restores as it starts; none by default. Only that host:
    ///        restartHost()'s is a run with nothing to restore.
    /// @param startupLayout The tabs the host opens on construction; none by default.
    explicit DaemonFixture(std::shared_ptr<vthost::IExtensionHosting> extensions = {},
                           vthost::NativeSessionServices services = {},
                           uint64_t instance = 1,
                           std::shared_ptr<vthost::ProfileCatalog const> profiles = {},
                           std::optional<vthost::WorkspaceRestore> restore = std::nullopt,
                           vtworkspace::Layout startupLayout = {}):
        extensions(std::move(extensions)),
        services(std::move(services)),
        instance(instance),
        profiles(std::move(profiles)),
        restore(std::move(restore)),
        startupLayout(std::move(startupLayout))
    {
        host = makeHost();
        auto listener = core::net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        port = (*listener)->boundPort();
        // Encrypt each accepted socket (server-side TLS, self-signed) before the
        // native protocol runs over it — the daemon's real TCP path. This exercises
        // the two-reactor TLS handshake (client + daemon on independent reactors),
        // which NativeClient's concurrent read+write drives.
        auto tls = core::net::makeSelfSignedServerContext();
        REQUIRE(tls.has_value());
        native = vthost::makeNativeHandler(loop, *host, this->services);
        auto const& context = *tls;
        auto handler = [this, context](vthost::ConnectionId id, std::unique_ptr<core::net::ISocket> socket) {
            auto gate = std::make_unique<GatedSocket>(
                std::make_unique<DroppableSocket>(context->wrap(std::move(socket), loop), live), gated);
            if (holdNewAtLayout)
                gate->holdAtNextLayout();
            return native(std::move(id), std::move(gate));
        };
        server = std::make_unique<vthost::ConnectionAcceptor>(loop, "test", std::move(*listener), handler);
        thread = std::thread { [this] {
            try
            {
                loop.blockOn(server->serve());
            }
            catch (core::async::OperationCancelled const&)
            {
                cancelled = true; // teardown cancelled the accept loop
            }
        } };
    }

    ~DaemonFixture()
    {
        loop.post([this] {
            server->close();
            loop.requestStop();
        });
        thread.join();
        // The accept loop has returned, but the connection flows it spawned may still be parked.
        // End them now, while the SessionHost they subscribe to still exists; ~EventLoop runs after
        // the host is gone.
        {
            auto const drain = vthost::LoopDrain { loop };
        }
    }

    DaemonFixture(DaemonFixture const&) = delete;
    DaemonFixture& operator=(DaemonFixture const&) = delete;
    DaemonFixture(DaemonFixture&&) = delete;
    DaemonFixture& operator=(DaemonFixture&&) = delete;

    /// The endpoint a client dials: loopback TCP + TLS (TOFU), no token.
    [[nodiscard]] vthost::TcpEndpoint endpoint() const
    {
        return vthost::TcpEndpoint { .host = "127.0.0.1", .port = port, .token = {}, .caPem = {} };
    }

    /// Runs @p fn on the daemon's loop thread and waits for its result —
    /// SessionHost is confined to that thread.
    template <typename F>
    auto onDaemon(F&& fn) -> decltype(fn())
    {
        auto promise = std::promise<decltype(fn())> {};
        auto future = promise.get_future();
        loop.post([&promise, fn = std::forward<F>(fn)]() mutable { promise.set_value(fn()); });
        return future.get();
    }

    /// Drops every connection, as a daemon that went away would, and keeps serving: what a client reconnects
    /// to finds the same host, sessions and all.
    void dropConnections()
    {
        onDaemon([this] {
            for (auto* socket: live)
                socket->close();
            return 0;
        });
    }

    /// Replaces the host with a new run's, as a restarted daemon: every window and session gone, ids counting
    /// from 1 again, and another instance stated.
    void restartHost()
    {
        whileDisconnected([this] {
            if (instance != 0)
                ++instance;
            host = makeHost();
        });
    }

    /// Drops every connection, runs @p change on the daemon's thread once none is left, and only then serves
    /// again: a client reconnecting meanwhile finds nothing, and so meets the daemon as @p change left it.
    template <typename F>
    void whileDisconnected(F change)
    {
        refuseConnections();
        serveAgain(std::move(change));
    }

    /// Drops every connection and closes each new one as it is accepted, until serveAgain(): a client
    /// reconnecting meanwhile keeps retrying.
    void refuseConnections()
    {
        onDaemon([this] {
            native = [](vthost::ConnectionId,
                        std::unique_ptr<core::net::ISocket> socket) -> core::async::Task<void> {
                socket->close();
                co_return;
            };
            for (auto* socket: live)
                socket->close();
            return 0;
        });
        // The connections' flows hold the host until they ended.
        REQUIRE(waitUntilOnDaemon([this] { return live.empty(); }));
    }

    /// Runs @p change on the daemon's thread, then serves connections again (@see refuseConnections).
    template <typename F = void (*)()>
    void serveAgain(F change = [] {})
    {
        onDaemon([this, &change] {
            change();
            native = vthost::makeNativeHandler(loop, *host, services);
            return 0;
        });
    }

    /// Polls @p predicate on the daemon's thread until it holds, for up to two seconds.
    template <typename F>
    [[nodiscard]] bool waitUntilOnDaemon(F predicate)
    {
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 400))
        {
            if (onDaemon(predicate))
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    /// @return What the pty of @p session has been sent so far.
    [[nodiscard]] std::string ptyInput(vtworkspace::SessionId session)
    {
        return onDaemon([this, session] {
            auto& tapped = dynamic_cast<vthost::TappingPty&>(host->terminal(session)->device());
            return std::string { dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer() };
        });
    }

    /// Has every connection let frames through up to and including the next LayoutState of @p window, and
    /// hold the rest until releaseHeld().
    void holdAfterLayoutOf(uint64_t window)
    {
        onDaemon([this, window] {
            for (auto* socket: gated)
                socket->holdAfterLayoutOf(window);
            return 0;
        });
    }

    /// Has every connection accepted from now on let its frames through up to its first layout, and hold
    /// that and the rest until releaseHeld().
    void holdNewConnectionsAtLayout()
    {
        onDaemon([this] {
            holdNewAtLayout = true;
            return 0;
        });
    }

    /// Sends what every connection held, and holds nothing more.
    void releaseHeld()
    {
        onDaemon([this] {
            holdNewAtLayout = false;
            for (auto* socket: gated)
                socket->release(loop);
            return 0;
        });
    }

    /// Seeds one session and writes @p text on its terminal.
    [[nodiscard]] vtworkspace::SessionId seedSession(std::string text)
    {
        return onDaemon([this, text = std::move(text)] {
            host->createTab();
            auto const id = host->model().window(host->windowId())->activeTab()->rootPane()->session();
            host->terminal(id)->writeToScreen(text);
            return id;
        });
    }
};

/// Whether a wait runs the GUI thread's queued work meanwhile.
enum class GuiEvents : uint8_t
{
    Pumped = 0, ///< Queued work runs, as it would while the GUI waits in its event loop.
    Held,       ///< Nothing queued runs: what the wait sees is what the reactor alone did.
};

/// Polls @p predicate at 5ms intervals until it holds or @p attempts elapse, running the GUI thread's
/// queued work in between unless @p events holds it. Also used inverted, to assert a state is never
/// reached.
[[nodiscard]] bool waitUntil(std::function<bool()> const& predicate,
                             int attempts = 400,
                             GuiEvents events = GuiEvents::Pumped)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, attempts))
    {
        if (events == GuiEvents::Pumped)
            QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

/// Runs every queued GUI step, and the deletions they leave: what a closed pane's teardown needs.
void drainGuiEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

/// A page size in the (columns, lines) order these tests read in.
[[nodiscard]] vtpty::PageSize cells(int columns, int lines)
{
    return vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) };
}

/// @return The daemon windows of the layouts a reconcile reads (NativeController::settledLayouts), ascending.
[[nodiscard]] std::vector<uint64_t> settledWindows(contour::remote::NativeController const& controller)
{
    return std::ranges::to<std::vector>(controller.settledLayouts() | std::views::keys);
}

/// The router's default factory, unreachable in these tests: attach mode always installs a delegate.
struct UnusedDefaultFactory final: contour::session::SessionFactory
{
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string>,
                                                        std::optional<vtbackend::PageSize>,
                                                        std::optional<vtpty::Process::ExecInfo>,
                                                        std::optional<std::string>) override
    {
        return nullptr;
    }
};

/// The production route to a `NativeController`.
///
/// The app never holds a controller as its factory: it holds a `RoutingSessionFactory` for its whole
/// life and swaps the DELEGATE when attach mode starts, so every call a session makes travels through
/// the router. Tests used to call the controller directly, which skips it — and a verb the router
/// forgets to forward is then invisible, because the base `SessionFactory` gives every verb a no-op
/// default. That is exactly how `bindTerminal` came to be dropped: a daemon-hosted pane rendered
/// nothing at all in the real GUI while every test here passed. Going through the router is what
/// makes these tests able to see it.
struct AttachRoute
{
    contour::remote::RoutingSessionFactory router { std::make_unique<UnusedDefaultFactory>() };

    explicit AttachRoute(contour::remote::NativeController& controller) { router.setDelegate(&controller); }

    AttachRoute(AttachRoute const&) = delete;
    AttachRoute& operator=(AttachRoute const&) = delete;
    AttachRoute(AttachRoute&&) = delete;
    AttachRoute& operator=(AttachRoute&&) = delete;
    ~AttachRoute() = default;
};

/// The local half of a mirrored pane: the terminal a GUI session builds around a router-issued
/// pty, wired up exactly as `TerminalSessionManager` wires it.
///
/// Building the terminal is not scaffolding around the thing under test — it IS the production path.
/// The controller populates this terminal's GRID rather than feeding reconstructed escape sequences
/// into the pty, so `createPty` alone mirrors nothing; `bindTerminal` is what starts it, and the
/// grid is where the answer shows up.
struct MirrorPane
{
    vtbackend::Terminal::NullEvents events;
    std::unique_ptr<vtbackend::Terminal> terminal;

    // SELF-REFERENTIAL, hence immovable: the terminal holds a reference to `events` above, so a move
    // would leave it pointing at the source object. Construct in place (optional::emplace), never
    // `std::optional { MirrorPane { ... } }` or a by-value return — either moves a temporary, and the
    // terminal then calls into a destroyed event listener from the reactor thread. Hold it by
    // unique_ptr where it has to be resettable.
    MirrorPane(MirrorPane const&) = delete;
    MirrorPane& operator=(MirrorPane const&) = delete;
    MirrorPane(MirrorPane&&) = delete;
    MirrorPane& operator=(MirrorPane&&) = delete;
    ~MirrorPane() = default;

    /// @param factory The ROUTER, not the controller — @see AttachRoute.
    /// @param pty The pty that factory handed out.
    /// @param interruptions Where the session is told what a restart stopped; none by default.
    MirrorPane(contour::session::SessionFactory& factory,
               std::unique_ptr<vtpty::Pty> pty,
               contour::session::InterruptionSink const& interruptions = {})
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = pty->pageSize();
        auto* device = pty.get();
        terminal = std::make_unique<vtbackend::Terminal>(events,
                                                         core::defaultEnvironment(),
                                                         core::platform::NativeFileSystem::instance(),
                                                         std::move(pty),
                                                         std::move(settings),
                                                         std::nullopt,
                                                         vtbackend::TerminalClocks::system(),
                                                         vtbackend::randomSemanticBlockTokens());
        factory.bindTerminal(device, *terminal, interruptions);
    }

    [[nodiscard]] vtpty::Pty& pty() const noexcept { return terminal->device(); }

    /// @return True once @p needle shows on the mirrored page. Under the terminal lock: the
    ///         controller populates the grid from its reactor thread.
    [[nodiscard]] bool shows(std::string_view needle) const
    {
        return waitUntil([&] {
            auto const guard = std::lock_guard { *terminal };
            return terminal->primaryScreen().grid().renderMainPageText().contains(needle);
        });
    }
};

/// Connects @p controller to the fixture's daemon, failing the test with the
/// controller's own reason (a timeout, a closed connection, a TLS error) rather
/// than a bare `false` -- the difference between a diagnosable CI failure and a
/// guess.
void requireConnected(contour::remote::RemoteController& controller, std::chrono::milliseconds timeout = 10s)
{
    auto const connected = controller.connectAndWait(timeout);
    INFO("connectAndWait: " << (connected ? std::string { "ok" } : connected.error()));
    REQUIRE(connected.has_value());
}

/// Connects @p controller, waits for the daemon's first layout, and realizes it into @p window.
/// @return The realized tab, or nullptr if no layout ever arrived.
[[nodiscard]] vtworkspace::Tab* attachAndRealize(contour::remote::NativeController& controller,
                                                 contour::session::TerminalSessionManager& manager,
                                                 vtworkspace::WindowId window)
{
    requireConnected(controller);
    if (!waitUntil([&] { return controller.settledLayout() != nullptr; }))
        return nullptr;
    contour::remote::applyRemoteLayout(manager, window, controller);
    return manager.model().window(window)->tabAt(0);
}
} // namespace

/// Renders a PageSize in Catch2 failure output (it would print `{?}` otherwise), through the
/// formatter vtbackend already defines for it.
template <>
struct Catch::StringMaker<vtpty::PageSize>
{
    static std::string convert(vtpty::PageSize value) { return std::format("{}", value); }
};

namespace
{

/// A factory that can never back a session (the attach guard's stand-in).
struct RefusingFactory final: contour::session::SessionFactory
{
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string> /*cwd*/,
                                                        std::optional<vtbackend::PageSize> pageSize,
                                                        std::optional<vtpty::Process::ExecInfo> /*command*/,
                                                        std::optional<std::string> /*profile*/) override
    {
        return std::make_unique<vtpty::MockPty>(
            pageSize.value_or(vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) }));
    }
    [[nodiscard]] bool canCreateSession() const noexcept override { return false; }
};

} // namespace

TEST_CASE("attach controller mirrors a remote session over a real socket", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("hello attach");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());
    REQUIRE(controller.pendingCount() == 1);
    REQUIRE(controller.canCreateSession());
    CHECK(controller.hosting() == contour::session::SessionHosting::Remote);

    // Through the ROUTER, exactly as the app does — see AttachRoute.
    auto route = AttachRoute { controller };
    // The factory hands out a pty bound to the remote session...
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    CHECK(!controller.canCreateSession()); // the one pending session is consumed

    // ...and once the terminal built around it is announced, the mirror populates its grid with
    // the remote screen.
    auto pane = MirrorPane { route.router, std::move(pty) };
    CHECK(pane.shows("hello attach"));

    // Input written by the terminal reaches the remote PTY.
    std::ignore = pane.pty().write("ls\r");
    CHECK(waitUntil([&] {
        return daemon.onDaemon([&] {
            auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
            return dynamic_cast<vtpty::MockPty&>(tapped.inner()).stdinBuffer();
        }) == "ls\r";
    }));

    // A local resize proposes the client area upstream. waitUntil pumps the Qt event loop, which
    // the geometry report needs: it is coalesced through a queued call (@see reportPaneGeometry).
    pane.pty().resizeScreen(cells(100, 40));
    CHECK(waitUntil(
        [&] { return daemon.onDaemon([&] { return daemon.host->pageSize(); }) == cells(100, 40); }));

    // Detaching closes the bound pty: its session would see a shell exit.
    controller.stop();
    CHECK(pane.pty().isClosed());
    pane.terminal.reset(); // unbind (the controller outlives its ptys' registrations)
}

TEST_CASE("NativeController.interruption.postedToTheSessionsThread", "[attach][controller][interrupted]")
{
    // The daemon restored a pane over the command its restart stopped. The mirror learns it on the reactor
    // thread; the session it belongs to is told on the GUI thread (this one), by a posted call.
    auto const snapshot = vthost::WorkspaceSnapshot {
        .takenAt = std::chrono::sys_seconds { std::chrono::seconds { 1'761'000'000 } },
        .windows = { vthost::WindowSnapshot {
            .tabs = { vthost::TabSnapshot {
                .root = vthost::PaneNode { .pane = vthost::PaneSnapshot { .runningCommandLine =
                                                                              "make test" } } } } } }
    };
    auto daemon =
        DaemonFixture { {}, {}, 1, {}, vthost::WorkspaceRestore { .snapshot = snapshot, .divider = {} } };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(controller.pendingCount() == 1);
    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);

    auto context = QObject {}; // the session's stand-in: it lives on this thread, the GUI's
    auto told = std::vector<std::optional<vtbackend::CommandLineText>> {};
    auto threads = std::vector<std::thread::id> {};
    auto pane = MirrorPane { route.router,
                             std::move(pty),
                             contour::session::InterruptionSink {
                                 .context = &context,
                                 .deliver = [&](std::optional<vtbackend::CommandLineText> commandLine) {
                                     told.push_back(std::move(commandLine));
                                     threads.push_back(std::this_thread::get_id());
                                 } } };

    REQUIRE(waitUntil([&] { return !told.empty(); }));
    REQUIRE(told.size() == 1);
    CHECK(told.front() == vtbackend::CommandLineText { "make test" });
    // Posted, not called from the reactor.
    CHECK(threads.front() == std::this_thread::get_id());

    // The shell's next command starts on the daemon: the daemon forgets it, the mirror follows, and the
    // session is told it is gone -- once.
    auto const session = vtworkspace::SessionId { 1 };
    std::ignore = daemon.onDaemon([&] {
        auto& terminal = *daemon.host->terminal(session);
        auto& device = dynamic_cast<vthost::TappingPty&>(terminal.device());
        dynamic_cast<vtpty::MockPty&>(device.inner())
            .appendStdOutBuffer("\033]133;A\033\\$ \033]133;B\033\\\033]133;C\033\\");
        std::ignore = terminal.processInputOnce();
        return 0;
    });
    CHECK(waitUntil([&] { return told.size() == 2; }));
    REQUIRE(told.size() == 2);
    CHECK_FALSE(told.back().has_value());
    CHECK_FALSE(waitUntil([&] { return told.size() > 2; }, /*attempts*/ 20));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("NativeController.extension.permission", "[attach][controller][extensions]")
{
    // A daemon's extension asked while no client was attached: the attaching client hands the question to
    // the pane it is asked in, and sends the user's answer back, which the daemon records.
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto daemon = DaemonFixture { hosting };
    std::ignore = daemon.seedSession("hello");
    auto const asked = vthost::proto::PermissionRequest {
        .session = 0,
        .hostId = "git@/usr/share/contour/extensions/git.wasm",
        .name = "git",
        .version = 1,
        .digest = "5f3c",
        .commands = { vthost::proto::WireDeclaredCommand { .name = "status", .argv = { "git", "status" } } },
    };
    REQUIRE(daemon.onDaemon([&] {
        hosting->sessions.front()->askPermission(asked);
        return true;
    }));

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    auto pane = MirrorPane { route.router, std::move(pty) };

    auto taken = std::vector<contour::remote::NativeController::HostedPermissionRequest> {};
    REQUIRE(waitUntil([&] {
        auto more = controller.takePermissionRequests();
        taken.insert(taken.end(), more.begin(), more.end());
        return !taken.empty();
    }));
    REQUIRE(taken.size() == 1);
    CHECK(taken.front().pty == &pane.pty());
    CHECK(taken.front().request.name == "git");
    CHECK(taken.front().request.hostId == asked.hostId);
    CHECK(taken.front().request.commands == asked.commands);

    controller.answerPermission(asked.hostId, asked.digest, vthost::GrantState::Granted);
    CHECK(waitUntil([&] {
        return daemon.onDaemon([&] { return hosting->lookup(asked.hostId, asked.digest); })
               == vthost::GrantState::Granted;
    }));
    CHECK(daemon.onDaemon([&] { return daemon.host->pendingPermissionRequests().empty(); }));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a GUI detach ends the daemon's TLS connection as a disconnect", "[attach][controller]")
{
    // stop() is the GUI's detach: stopMuxReactor posts the detach and only then cancels the loop.
    // Cancelled too early, the client's orderly close -- close_notify over TLS -- is unwound half
    // done, and the daemon logs an ordinary detach as a truncated read.
    auto capture = SharedLogCapture {};
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("detach");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    REQUIRE(controller.connectAndWait(10s).has_value());
    controller.stop();

    // The daemon's side of the connection ends a few turns later, on its own thread.
    CHECK(waitUntil([&] { return capture.contains(": disconnected") || capture.contains("read failed"); }));
    INFO(capture.text());
    CHECK(capture.contains(": disconnected"));
    CHECK_FALSE(capture.contains("read failed"));
}

// B2 foundation: the controller captures the daemon's authoritative tab/pane
// tree (the daemon pushes LayoutState leading the attach snapshot), so the GUI
// can reconstruct its own split tree from it rather than flattening one tab per
// session.
TEST_CASE("attach controller captures the daemon's tab and pane layout", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("one");
    // Split the seeded tab so the layout carries a non-trivial pane tree.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());

    // The layout is pushed leading the snapshot; poll until the controller has it.
    auto layout = std::optional<vthost::proto::LayoutState> {};
    for (auto i = 0; i < 200 && !(layout = controller.layout()).has_value(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(layout.has_value());
    REQUIRE(layout->tabs.size() == 1);

    // The tab's root is a vertical split with two distinct-session leaves.
    auto const& root = layout->tabs.front().root;
    CHECK(root.split == std::to_underlying(vtworkspace::SplitState::Vertical));
    REQUIRE(root.children.size() == 2);
    CHECK(root.children[0].session != 0);
    CHECK(root.children[1].session != 0);
    CHECK(root.children[0].session != root.children[1].session);

    controller.stop();
}

// B2 executor end-to-end: a split daemon layout is realized as a real 2-pane tab
// in the GUI's own SessionModel — over a real TCP+TLS attach connection, driven by
// the shared applyRemoteLayout. Each pane binds to its remote session through the
// beforeLeafSeed → setNextBindSession seam (the path createPty takes here).
TEST_CASE("attach realizes a split daemon layout as a 2-pane tab", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("root");
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.6);
        return 0;
    });

    // The NativeController is the manager's session factory (attach mode).
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    // The layout leads the snapshot; poll until the split tree has arrived.
    for (auto i = 0; i < 200; ++i)
    {
        auto const l = ac->settledLayout();
        if (l && l->tabs.size() == 1 && l->tabs.front().root.children.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }
    auto const wl = ac->wireLayout();
    REQUIRE(wl.layout.tabs.size() == 1);

    // Realize the daemon's tree into the GUI window.
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);

    auto* window = app.manager().model().window(win.id);
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 1);
    auto* tab = window->tabAt(0);
    REQUIRE(tab != nullptr);
    CHECK(tab->paneCount() == 2); // the daemon's split reproduced locally
    REQUIRE_FALSE(tab->rootPane()->isLeaf());
    CHECK(tab->rootPane()->splitState() == vtworkspace::SplitState::Vertical);

    ac->stop();
}

// B3-Qt: a split authored on the daemon AFTER attach reconciles into the already-
// shown tab as a second pane (intra-tab incremental reconciliation), rather than
// only whole new tabs.
TEST_CASE("attach reconciles a split authored on the daemon after attach", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    // The settled layouts, as applyRemoteLayout realizes them: the push that brings a layout has ended.
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    // Split the tab on the daemon (after the client attached) and wait for the
    // split to reach the client's layout.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Horizontal, 0.5);
        return 0;
    });
    REQUIRE(waitUntil([&] { return ac->settledLayout()->tabs.front().root.isSplit(); }));

    // Reconcile: the already-shown tab grows a second pane.
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);
    CHECK(app.manager().model().window(win.id)->tabCount() == 1); // it stayed one tab, now split

    ac->stop();
}

namespace
{
/// The attach controller as the manager's session factory, made to refuse new sessions on demand: what a
/// factory that cannot back one right now says to a split the reconcile asks for.
class SplitRefusingFactory final: public contour::session::SessionFactory
{
  public:
    explicit SplitRefusingFactory(std::unique_ptr<contour::remote::NativeController> controller):
        _controller(std::move(controller))
    {
    }

    [[nodiscard]] contour::remote::NativeController& controller() const noexcept { return *_controller; }

    /// From now on no new session can be backed.
    void refuse() noexcept { _refusing = Refusing::Yes; }

    [[nodiscard]] bool canCreateSession() const noexcept override
    {
        return _refusing == Refusing::No && _controller->canCreateSession();
    }
    [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
    {
        return _controller->hosting();
    }
    [[nodiscard]] std::optional<uint64_t> hostSessionIdOf(vtpty::Pty const& pty) const noexcept override
    {
        return _controller->hostSessionIdOf(pty);
    }
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string> cwd,
                                                        std::optional<vtbackend::PageSize> pageSize,
                                                        std::optional<vtpty::Process::ExecInfo> command,
                                                        std::optional<std::string> profileName) override
    {
        return _controller->createPty(std::move(cwd), pageSize, std::move(command), std::move(profileName));
    }
    void bindTerminal(vtpty::Pty const* pty,
                      vtbackend::Terminal& terminal,
                      contour::session::InterruptionSink const& interruptions) override
    {
        _controller->bindTerminal(pty, terminal, interruptions);
    }

  private:
    enum class Refusing : uint8_t
    {
        No = 0,
        Yes,
    };
    std::unique_ptr<contour::remote::NativeController> _controller;
    std::atomic<Refusing> _refusing = Refusing::No;
};
} // namespace

// A split the reconcile cannot realize -- the factory backs no new session right now -- is left to the next
// push: the reconcile returns, rather than finding the same split again for ever.
TEST_CASE("a daemon split the factory refuses ends the reconcile, leaving it for the next push",
          "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");
    auto timeline = TestTimeline {};
    auto factoryOwned =
        std::make_unique<SplitRefusingFactory>(makeNativeController(daemon.endpoint(), timeline));
    auto* const factory = factoryOwned.get();
    auto& ac = factory->controller();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(ac);
    REQUIRE(waitUntil([&] { return ac.settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Horizontal, 0.5);
        return 0;
    });
    REQUIRE(waitUntil([&] { return ac.settledLayout()->tabs.front().root.isSplit(); }));
    factory->refuse();

    // Returns: the split is not realized, and the tab is as it was.
    contour::remote::applyRemoteLayout(app.manager(), win.id, ac);
    CHECK(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);
    // And no session is left awaiting a pane: the next split the user makes here binds nothing of the
    // daemon's.
    CHECK_FALSE(ac.canCreateSession());
    ac.stop();
}

// The incremental reconcile must reproduce the daemon split's ACTUAL ratio, not a
// hard-coded even split — a non-even split authored on the daemon should mirror
// with matching proportions (regression: the SplitOp discarded node.ratio).
TEST_CASE("attach reconciles an uneven daemon split with matching proportions", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && ac->settledLayout() == nullptr; ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    // A distinctly uneven split (0.7 to the first/acting child) authored on the daemon.
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.7);
        return 0;
    });
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->settledLayout(); l && !l->tabs.front().root.children.empty())
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE_FALSE(ac->settledLayout()->tabs.front().root.children.empty());

    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* root = app.manager().model().window(win.id)->tabAt(0)->rootPane();
    REQUIRE_FALSE(root->isLeaf());
    // The first child's share is ~0.7, NOT the 0.5 default the dropped ratio produced.
    CHECK(root->ratio() > 0.6);
    CHECK(root->ratio() < 0.8);

    ac->stop();
}

// B3-Qt: a pane closed on the daemon (here, or by another client) is removed
// locally by the subtractive reconciler — the remote session leaves the layout,
// so its local pane is terminated.
TEST_CASE("attach reconciles a pane closed on the daemon by removing it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");
    auto const second = daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return tab->activePane()->session(); // the new (active) pane's session
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    for (auto i = 0; i < 200 && ac->settledLayout() == nullptr; ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);

    // Close the second pane ON THE DAEMON; wait for the layout to drop it.
    daemon.onDaemon([&daemon, second] {
        daemon.host->handleSessionExit(second);
        return 0;
    });
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->settledLayout(); l && l->tabs.front().root.children.empty())
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(ac->settledLayout()->tabs.front().root.children.empty());

    // Reconcile: the local pane for the vanished session is terminated (async
    // teardown, so pump the event loop until the tab is back to a single pane).
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto const closed = [&] {
        for (auto i = 0; i < 200; ++i)
        {
            QCoreApplication::processEvents();
            if (app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1)
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }();
    CHECK(closed);
    // The surviving pane's session is untouched: reconciling a daemon-side close must not take
    // the rest of the tab down with it. (The redundant ClosePane the reconciler used to echo for
    // the ALREADY-removed session is not separately observable — session ids are monotonic, so an
    // echoed id can never name a live session — which is why this asserts the survivor instead.)
    CHECK(daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); }) == 1);

    ac->stop();
}

// Releasing a pty is NOT ending its session. `unbind()` runs from the pty's destructor, which
// cannot tell a pane close from a window close, an app quit or a lost connection — so it must only
// forget the binding. Authoring a close from there (what it used to do, gated on a `_stopped` flag
// that is set far too late to mean anything) got the answer wrong in both directions: nothing that
// ends a session destroyed a pty early enough for it to fire, so no close was ever authored at all,
// and any future change that DID delete a session on window close would silently have started
// killing the user's shells. This pins the contract from the side a test can drive directly.
TEST_CASE("releasing a mirrored pty leaves its remote session running", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("keep me running");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(controller.pendingCount() == 1);
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };
    REQUIRE(liveSessions() == 1);

    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    auto pane = std::make_unique<MirrorPane>(route.router, std::move(pty));
    CHECK(pane->shows("keep me running"));

    // The pane's pty goes away with the connection still up. Inverted wait: give any ClosePane a
    // generous window to cross the wire and be applied, then assert the session never died.
    pane.reset();
    CHECK_FALSE(waitUntil([&] { return liveSessions() == 0; }));
    CHECK(liveSessions() == 1);

    controller.stop();
}

// The user-visible half of the same defect, and the counterpart of the case above: closing a PANE
// really does end its session, remotely exactly as locally. It did not before — the close was
// authored only from the pty destructor, and a TerminalSession is a QObject child of the manager
// that nothing deletes until app teardown, so the ClosePane never went out. A pane closed while
// attached stayed alive on the daemon with no view, and reappeared on the next attach.
TEST_CASE("the ClosePane action ends a remote session like a local one", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->settledLayout();
            l && l->tabs.size() == 1 && l->tabs.front().root.children.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };
    REQUIRE(liveSessions() == 2);

    // The ClosePane action's own path: TerminalSession::operator()(actions::ClosePane) forwards
    // here, and the acting session names the tab whose ACTIVE pane closes.
    app.manager().closeActivePane(win->activeSession(), contour::session::SessionEnd::Destroy);

    CHECK(waitUntil([&] { return liveSessions() == 1; }));
    CHECK(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 1);

    ac->stop();
}

TEST_CASE("an action that changes hosted state is performed by the daemon, and the mirror follows",
          "[attach][controller][operations][threading]")
{
    static_assert(contour::remote::toWireOperation(contour::session::HostedOperation::WriteScreen)
                  == vthost::proto::EngineOperation::WriteScreen);

    auto daemon = DaemonFixture {};
    auto rows = std::string {};
    for (auto const row: std::views::iota(0, 60))
        rows += std::format("row {}\r\n", row);
    auto const session = daemon.seedSession(rows + "\033]133;A\033\\$ prompt-here");
    auto const daemonHistory = [&] {
        return daemon.onDaemon(
            [&] { return daemon.host->terminal(session)->primaryScreen().historyLineCount(); });
    };
    REQUIRE(daemonHistory() > vtbackend::LineCount(0));

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* pane = win->activeSession();
    REQUIRE(pane != nullptr);
    auto const& terminal = pane->terminal();
    auto const& mirror = terminal.primaryScreen();
    // Under the terminal lock: the controller writes the mirror from its reactor thread.
    auto const mirrorShows = [&](auto const& predicate) {
        return waitUntil([&] {
            auto const guard = std::lock_guard { terminal };
            return predicate();
        });
    };
    REQUIRE(mirrorShows([&] { return mirror.historyLineCount() > vtbackend::LineCount(0); }));

    // The action's own path, as a key binding or the context menu runs it.
    CHECK((*pane)(contour::actions::ClearToPrompt {}));

    CHECK(waitUntil([&] { return daemonHistory() == vtbackend::LineCount(0); }));
    CHECK(mirrorShows([&] { return mirror.historyLineCount() == vtbackend::LineCount(0); }));
    CHECK(mirrorShows(
        [&] { return mirror.grid().lineText(vtbackend::LineOffset(0)).starts_with("$ prompt-here"); }));

    ac->stop();
}

TEST_CASE("an action that changes hosted state does not report success without a daemon",
          "[attach][controller][operations]")
{
    // Never connected, as after the daemon went away: there is nowhere for the operation to run that would
    // stick, so it is not "sent".
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} }, timeline);
    auto& controller = *controllerOwned;
    auto const pty =
        vtpty::MockPty { vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) } };
    for (auto const& row: contour::session::HostedOperationTable)
        CHECK(controller.requestHostedOperation(
                  &pty, contour::session::HostedOperationRequest { .operation = row.operation })
              == contour::session::HostedOperationRoute::Unreachable);
}

namespace
{

/// Types @p keys into @p session, each one pressed and released, as a keyboard delivers them.
void typeKeys(contour::session::TerminalSession& session, std::string_view keys)
{
    for (auto const key: keys)
    {
        auto const ch = static_cast<char32_t>(key);
        auto const identity =
            vtbackend::KeyIdentity { .unshiftedKey = ch, .nativeVirtualKey = static_cast<uint32_t>(ch) };
        for (auto const eventType:
             { vtbackend::KeyboardEventType::Press, vtbackend::KeyboardEventType::Release })
            session.sendCharEvent(
                ch, identity, vtbackend::KeyboardModifiers {}, eventType, std::chrono::steady_clock::now());
    }
}

/// The rows of @p terminal's page that carry a vi mark, by their text.
[[nodiscard]] std::vector<std::string> markedRows(vtbackend::Terminal const& terminal)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto marked = std::vector<std::string> {};
    for (auto const row: std::views::iota(0, unbox<int>(terminal.pageSize().lines)))
        if (grid.lineAt(vtbackend::LineOffset(row)).isFlagEnabled(vtbackend::LineFlag::UserMark))
        {
            auto text = grid.lineText(vtbackend::LineOffset(row));
            text.erase(text.find_last_not_of(' ') + 1);
            marked.push_back(std::move(text));
        }
    return marked;
}

} // namespace

TEST_CASE("a vi mark on an attached pane is the daemon's, and the mirror shows it",
          "[attach][controller][operations][vi]")
{
    static_assert(contour::remote::toWireOperation(contour::session::HostedOperation::SetUserMark)
                  == vthost::proto::EngineOperation::SetUserMark);

    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("first\r\nsecond\r\nthird\r\n$ ");
    auto const daemonMarks = [&] {
        return daemon.onDaemon([&] { return markedRows(*daemon.host->terminal(session)); });
    };

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* pane = win->activeSession();
    REQUIRE(pane != nullptr);
    auto const& terminal = pane->terminal();
    // Under the terminal lock: the controller writes the mirror from its reactor thread.
    auto const mirrorMarks = [&] {
        auto const guard = std::lock_guard { terminal };
        return markedRows(terminal);
    };
    REQUIRE(waitUntil([&] {
        auto const guard = std::lock_guard { terminal };
        return terminal.primaryScreen().grid().renderMainPageText().contains("third");
    }));

    // The keys' own path, as the keyboard delivers them in normal mode: up from the prompt to "second".
    (*pane)(contour::actions::ViNormalMode {});
    typeKeys(*pane, "kkmm");

    CHECK(waitUntil([&] { return daemonMarks() == std::vector<std::string> { "second" }; }));
    CHECK(waitUntil([&] { return mirrorMarks() == std::vector<std::string> { "second" }; }));

    // `mm` on the line the daemon marked takes the mark off there, and so here.
    typeKeys(*pane, "mm");
    CHECK(waitUntil([&] { return daemonMarks().empty(); }));
    CHECK(waitUntil([&] { return mirrorMarks().empty(); }));

    ac->stop();
}

TEST_CASE("the vi z keys fold an attached pane's command output", "[attach][controller][vi][folding]")
{
    // Folding is the client's view of the mirrored grid, as it is a local session's of its own: the keys
    // never leave the client. What they need from the daemon is the shell's marks, carried as row flags.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession(vtbackend::test::commandRun("ls", { "one", "two", "three" }) + "$ ");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* pane = win->activeSession();
    REQUIRE(pane != nullptr);
    auto& terminal = pane->terminal();
    // Under the terminal lock: the controller writes the mirror from its reactor thread.
    auto const hidden = [&] {
        auto const guard = std::lock_guard { terminal };
        return terminal.hiddenLineCount();
    };
    REQUIRE(waitUntil([&] {
        auto const guard = std::lock_guard { terminal };
        return terminal.primaryScreen().grid().renderMainPageText().contains("three");
    }));

    (*pane)(contour::actions::ViNormalMode {});
    typeKeys(*pane, "zM");
    CHECK(hidden() > vtbackend::LineCount(0));

    typeKeys(*pane, "zR");
    CHECK(hidden() == vtbackend::LineCount(0));

    ac->stop();
}

// Regression: what a client reports upstream after a split. `proto::ResizeRequest` names the whole
// CLIENT AREA — the daemon projects the tab's pane tree into it (SessionHost::reprojectLayouts ->
// vtworkspace::layoutInCells). Reporting one PANE's grid there halved the area on every split: a
// 100-column window split in two produced ~25-column remote grids inside 50-column local panes, so
// the right half of each pane rendered empty. And because the daemon projects by split RATIO while
// the GUI lays out in PIXELS, the client area alone cannot express a dragged divider — each pane's
// own grid has to reach its PTY (proto::ResizePane).
TEST_CASE("attach sizes remote panes to the panes the GUI renders", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("root");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    REQUIRE(ac->connectAndWait(10s).has_value());
    // Settled as well: applyRemoteLayout realizes the settled layouts, not the live ones.
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* tab = app.manager().model().window(win.id)->tabAt(0);
    REQUIRE(app.manager().sessionsOfTab(tab).size() == 1);

    auto const clientArea = [&] {
        return daemon.onDaemon([&] { return daemon.host->pageSize(); });
    };
    auto const remoteSize = [&](uint64_t session) {
        return daemon.onDaemon([&] {
            auto* terminal = daemon.host->terminal(vtworkspace::SessionId { session });
            return terminal != nullptr ? terminal->pageSize() : vtpty::PageSize {};
        });
    };

    // The window is 100x30 cells while still unsplit; the daemon adopts that as its client area.
    app.manager().sessionsOfTab(tab).front()->terminal().device().resizeScreen(cells(100, 30));
    REQUIRE(waitUntil([&] { return clientArea() == cells(100, 30); }));

    // Split it evenly on the daemon and reconcile the second pane in.
    daemon.onDaemon([&daemon] {
        auto* daemonTab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(daemonTab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });
    REQUIRE(waitUntil([&] {
        auto const pushed = ac->settledLayout();
        return pushed && pushed->tabs.front().root.children.size() == 2;
    }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().sessionsOfTab(tab).size() == 2);

    // Which remote session backs which local pane (first child = left, second = right). layout()
    // hands out a COPY, so keep it alive while its tree is read.
    auto const pushed = ac->settledLayout();
    auto const leftSession = pushed->tabs.front().root.children[0].session;
    auto const rightSession = pushed->tabs.front().root.children[1].session;
    auto const paneFor = [&](uint64_t remote) -> contour::session::TerminalSession* {
        for (auto* session: app.manager().sessionsOfTab(tab))
            if (ac->sessionForPty(&session->terminal().device()) == remote)
                return session;
        return nullptr;
    };
    auto* left = paneFor(leftSession);
    auto* right = paneFor(rightSession);
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);

    SECTION("the split alone must not shrink anything")
    {
        // What PaneNode.qml's SplitView hands the two panes of a 100-column window. The daemon
        // already projected exactly this, so nothing on it may move — which makes the assertion a
        // NEGATIVE one: waiting for the right state would pass on the state that is about to break.
        left->terminal().device().resizeScreen(cells(50, 30));
        right->terminal().device().resizeScreen(cells(49, 30));

        auto const moved = [&] {
            return clientArea() != cells(100, 30) || remoteSize(leftSession) != cells(50, 30)
                   || remoteSize(rightSession) != cells(49, 30);
        };
        CHECK_FALSE(waitUntil(moved, /*attempts=*/100));
        // Named individually, so a failure says which of the three collapsed.
        CHECK(clientArea() == cells(100, 30));
        CHECK(remoteSize(leftSession) == cells(50, 30));
        CHECK(remoteSize(rightSession) == cells(49, 30));
    }

    SECTION("a window resize after a split reports the composed area")
    {
        // The window grew to 120 columns; the SplitView hands the panes 60 and 59.
        left->terminal().device().resizeScreen(cells(60, 30));
        right->terminal().device().resizeScreen(cells(59, 30));

        CHECK(waitUntil([&] { return clientArea() == cells(120, 30); }));
        CHECK(waitUntil([&] { return remoteSize(leftSession) == cells(60, 30); }));
        CHECK(waitUntil([&] { return remoteSize(rightSession) == cells(59, 30); }));
    }

    SECTION("a dragged divider sizes each pane exactly")
    {
        // The user dragged the divider, so the panes no longer match the daemon's 0.5 ratio — whose
        // projection would still say 50 / 49. Each pane's own grid is what must reach its PTY.
        left->terminal().device().resizeScreen(cells(70, 30));
        right->terminal().device().resizeScreen(cells(29, 30));

        CHECK(waitUntil([&] { return remoteSize(leftSession) == cells(70, 30); }));
        CHECK(waitUntil([&] { return remoteSize(rightSession) == cells(29, 30); }));
        // ...while the client area still describes the whole window.
        CHECK(clientArea() == cells(100, 30));
    }

    ac->stop();
}

// B3-Qt: a GUI split in attach mode is authored on the daemon (routed by
// requestRemoteSplit), which splits the right pane and re-pushes its layout; the
// reconciler realizes the new pane locally — the full split-authoring loop.
TEST_CASE("attach authors a split on the daemon and reconciles it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && ac->settledLayout() == nullptr; ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto* tab = app.manager().model().window(win.id)->tabAt(0);
    REQUIRE(app.manager().sessionsOfTab(tab).size() == 1);

    // A GUI split of the only pane routes to the daemon (requestRemoteSplit).
    app.manager().splitActivePane(/*vertical=*/true, app.manager().sessionsOfTab(tab).front());
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->settledLayout(); l && !l->tabs.front().root.children.empty())
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE_FALSE(ac->settledLayout()->tabs.front().root.children.empty());

    // Reconcile: the local tab grows a second pane, still one tab.
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(app.manager().model().window(win.id)->tabAt(0)->paneCount() == 2);
    CHECK(app.manager().model().window(win.id)->tabCount() == 1);

    ac->stop();
}

// A tab dropped on a pane's edge is merged into the split BY THE DAEMON: it moves the tab's sessions into the
// target tab and re-pushes its layout, and the reconciler grafts the same tab here. Nothing is spawned or
// closed, so both sessions keep running on the daemon.
TEST_CASE("attach merges a tab into a split on the daemon, and the sessions live on",
          "[attach][controller][merge][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const target = daemon.seedSession("target");
    auto const dragged = daemon.seedSession("dragged");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 2; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto& manager = app.manager();
    auto* const localWindow = manager.model().window(win.id);
    REQUIRE(localWindow->tabCount() == 2);
    REQUIRE(manager.tabMergeSupport(win.id, win.id) == contour::session::TabMergeSupport::Supported);

    // The tabs in the daemon's order: [target, dragged]; drop the second on the first's right edge.
    auto* const targetTab = localWindow->tabAt(0);
    manager.mergeTabIntoSplit(
        win.id, 1, win.id, targetTab->id(), targetTab->rootPane()->id(), vtworkspace::SplitSide::Right);

    // Authored on the daemon, not made here: nothing moved until its layout arrives.
    CHECK(localWindow->tabCount() == 2);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 1; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);

    REQUIRE(localWindow->tabCount() == 1);
    auto* const merged = localWindow->tabAt(0);
    CHECK(merged->paneCount() == 2);
    CHECK(merged->rootPane()->splitState() == vtworkspace::SplitState::Vertical);
    REQUIRE(manager.sessionsOfTab(merged).size() == 2);
    // And on the daemon both sessions are still running, in one tab.
    CHECK(daemon.onDaemon([&] { return daemon.host->terminal(target) != nullptr; }));
    CHECK(daemon.onDaemon([&] { return daemon.host->terminal(dragged) != nullptr; }));
    CHECK(daemon.onDaemon([&] {
        return std::get<1>(daemon.host->model().findSessionLeaf(target))
               == std::get<1>(daemon.host->model().findSessionLeaf(dragged));
    }));

    ac->stop();
}

TEST_CASE("a tab dropped against a daemon without TabMerge is not offered a split",
          "[attach][controller][merge][compat][daemon]")
{
    using vthost::proto::Capability;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("one");
    std::ignore = daemon.seedSession("two");
    daemon.whileDisconnected([&] {
        daemon.services.handshake.capabilities = vthost::proto::CapabilitySet::of(
            { Capability::ExtensionPermissions, Capability::InitialClientArea, Capability::AttachIntent });
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 2; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    auto& manager = app.manager();

    REQUIRE(ac->negotiated().has_value());
    CHECK_FALSE(ac->negotiated()->shared.contains(Capability::TabMerge));
    // The tab is left to tear off, as it was before the verb existed: the window offers no split and the
    // controller routes nothing.
    CHECK(manager.tabMergeSupport(win.id, win.id) == contour::session::TabMergeSupport::Unsupported);
    auto* const first = manager.model().window(win.id)->tabAt(0);
    auto* const second = manager.model().window(win.id)->tabAt(1);
    auto const* const firstPty = &manager.sessionsOfTab(first).front()->terminal().device();
    auto const* const secondPty = &manager.sessionsOfTab(second).front()->terminal().device();
    // And a request made anyway is refused, never left for the manager to merge here: the daemon owns this
    // layout and would never hear of it.
    CHECK(ac->requestRemoteTabMerge(std::array { secondPty }, firstPty, vtworkspace::SplitSide::Right)
          == contour::session::RemoteRouting::Refused);
    // A pane that is not bound to a daemon session (yet) is no pane the daemon can name.
    CHECK(ac->requestRemoteTabMerge(
              std::array<vtpty::Pty const*, 1> { nullptr }, firstPty, vtworkspace::SplitSide::Right)
          == contour::session::RemoteRouting::Refused);

    ac->stop();
}

// A ratio only the CLIENT knows is a ratio the user loses: a re-attaching client rebuilds every tab
// from the daemon's LayoutState, so a divider the daemon never heard about comes back at the ratio
// the split was CREATED with. This is the whole point of proto::ResizeSplit — the drag has to reach
// the daemon's model, and the second attach has to read it back.
TEST_CASE("attach reports a moved divider, so a re-attaching client restores it", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    // The daemon's split ratio, or nullopt while its root is still a single leaf.
    auto daemonRatio = [&daemon]() -> std::optional<double> {
        return daemon.onDaemon([&daemon]() -> std::optional<double> {
            auto* root = daemon.host->model().window(daemon.host->windowId())->activeTab()->rootPane();
            return root->isLeaf() ? std::nullopt : std::optional { root->ratio() };
        });
    };

    {
        // The first client splits and then drags the divider well off centre.
        auto timeline = TestTimeline {};
        auto acOwned = makeNativeController(daemon.endpoint(), timeline);
        auto* ac = acOwned.get();
        contour::test::TestApp app { std::move(acOwned) };
        contour::test::ScopedController const win { app.manager() };

        auto* tab = attachAndRealize(*ac, app.manager(), win.id);
        REQUIRE(tab != nullptr);
        REQUIRE(app.manager().sessionsOfTab(tab).size() == 1);

        app.manager().splitActivePane(/*vertical=*/true, app.manager().sessionsOfTab(tab).front());
        // Wait for the split to reach THIS CLIENT's layout, not merely the daemon's model: the
        // daemon mutates its model before it pushes anything, so waiting on the daemon lets
        // applyRemoteLayout run against a LayoutState that predates the split — and it would then
        // find no new split to realize.
        REQUIRE(waitUntil([&] {
            auto const wire = ac->settledLayout();
            return wire != nullptr && !wire->tabs.empty() && wire->tabs.front().root.isSplit();
        }));
        contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
        REQUIRE(tab->paneCount() == 2);

        // The drag itself. Both ratio mutators land on paneRatioChanged, so this is the same route a
        // ResizePane keybinding takes.
        app.manager().setPaneRatio(tab->id(), tab->rootPane()->id(), 0.75);
        CHECK(tab->rootPane()->ratio() == 0.75);
        CHECK(waitUntil([&] { return daemonRatio() == 0.75; }));

        ac->stop();
    }

    // A brand-new client, with nothing claimed: applyRemoteLayout realizes the WHOLE tab from the
    // daemon's layout — the path that used to hand back an even split.
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* tab = attachAndRealize(*ac, app.manager(), win.id);
    REQUIRE(tab != nullptr);
    REQUIRE(tab->paneCount() == 2);
    CHECK(tab->rootPane()->ratio() == 0.75);

    ac->stop();
}

// With the ratio living on the daemon, a divider one client moves has to reach the others: their
// tabs are already realized, so the additive/subtractive passes have nothing to do and would leave
// them showing a layout the daemon no longer describes.
namespace
{
/// One GUI attached to a daemon: its controller, its app, and the window the daemon's first window is
/// realized into.
struct AttachedGui
{
    TestTimeline timeline;
    contour::remote::NativeController* controller;
    contour::test::TestApp app;
    contour::test::ScopedController window;

    explicit AttachedGui(DaemonFixture const& daemon):
        AttachedGui { makeNativeController(daemon.endpoint(), timeline) }
    {
    }

    /// The window's one tab.
    [[nodiscard]] vtworkspace::Tab& tab() { return *app.manager().model().window(window.id)->tabAt(0); }

    /// Reconciles what the daemon last pushed, as the app does on every push.
    void reconcile() { contour::remote::applyRemoteLayout(app.manager(), window.id, *controller); }

    /// @return The daemon's id for the session of @p leaf.
    [[nodiscard]] std::optional<uint64_t> remoteOf(vtworkspace::Pane const& leaf)
    {
        auto* session = app.manager().sessionForId(leaf.session());
        return session != nullptr ? controller->sessionForPty(&session->terminal().device()) : std::nullopt;
    }

  private:
    explicit AttachedGui(std::unique_ptr<contour::remote::NativeController> owned):
        controller { owned.get() }, app { std::move(owned) }, window { app.manager() }
    {
    }
};

/// @return How many EditTab frames the daemon received, by its protocol trace in @p log.
[[nodiscard]] std::ptrdiff_t editTabsReceived(std::string const& log)
{
    static auto const received = std::regex { R"(recv #\d+ EditTab )" };
    return std::distance(std::sregex_iterator(log.begin(), log.end(), received), std::sregex_iterator());
}
} // namespace

TEST_CASE("a second client sees the first client's zoom, title and colour", "[attach][controller][sync-back]")
{
    // Declared first: it outlives every thread that logs into it.
    auto capture = SharedLogCapture {};
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("root");
    daemon.onDaemon([&daemon] {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return 0;
    });

    auto a = AttachedGui { daemon };
    auto b = AttachedGui { daemon };
    for (auto* gui: { &a, &b })
    {
        requireConnected(*gui->controller);
        REQUIRE(waitUntil([&] {
            auto const layout = gui->controller->settledLayout();
            return layout && layout->tabs.size() == 1 && layout->tabs.front().root.children.size() == 2;
        }));
        gui->reconcile();
        REQUIRE(gui->tab().paneCount() == 2);
    }

    // What B shows, once the daemon pushed it and B reconciled the push.
    auto const bShows = [&](auto const& predicate) {
        return waitUntil([&] {
            b.reconcile();
            return predicate(b.tab());
        });
    };

    // A zooms its active pane: B is zoomed on the same session.
    auto& tabA = a.tab();
    auto const zoomed = a.remoteOf(*tabA.activePane());
    REQUIRE(zoomed.has_value());
    a.app.manager().model().toggleActivePaneZoom(tabA.id());
    CHECK(bShows([&](vtworkspace::Tab const& tab) {
        return tab.isZoomed() && b.remoteOf(*tab.activePane()) == zoomed;
    }));

    // A renames the tab, then colours it.
    a.app.manager().model().setTabTitle(tabA.id(), "build");
    CHECK(bShows([](vtworkspace::Tab const& tab) { return tab.runtimeTitle() == "build"; }));
    auto constexpr Color = vtbackend::RGBColor { 0x336699 };
    a.app.manager().model().setTabColor(tabA.id(), vtworkspace::TabColorSource::User, Color);
    CHECK(bShows(
        [&](vtworkspace::Tab const& tab) { return tab.color(vtworkspace::TabColorSource::User) == Color; }));

    // Every change was sent once, by A; neither client answered what it applied with an edit of its own. A's
    // own pushes are reconciled too, and settle without one.
    a.reconcile();
    b.reconcile();
    std::this_thread::sleep_for(100ms);
    CHECK(editTabsReceived(capture.text()) == 3);

    a.controller->stop();
    b.controller->stop();
}

TEST_CASE("a divider moved on the daemon re-flows an already realized tab", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("only");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* tab = attachAndRealize(*ac, app.manager(), win.id);
    REQUIRE(tab != nullptr);
    app.manager().splitActivePane(/*vertical=*/true, app.manager().sessionsOfTab(tab).front());
    REQUIRE(waitUntil([&] {
        auto const layout = ac->settledLayout();
        return layout && !layout->tabs.front().root.children.empty();
    }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(tab->paneCount() == 2);
    REQUIRE(tab->rootPane()->ratio() == 0.5);

    // What a SECOND client's ResizeSplit does to the daemon's model.
    auto const moved = daemon.onDaemon([&daemon] {
        auto* daemonTab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->model().setPaneRatio(daemonTab->id(), daemonTab->rootPane()->id(), 0.3);
        return true;
    });
    REQUIRE(moved);
    REQUIRE(waitUntil([&] {
        auto const layout = ac->settledLayout();
        return layout && layout->tabs.front().root.ratio == 3000;
    }));

    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(tab->rootPane()->ratio() == 0.3);

    // ...and settles there. The pass writes through the model, which re-emits paneRatioChanged; if
    // that echoed back the daemon would keep re-pushing, and two clients would trade a divider
    // forever. Comparing in wire units is what stops the second write from happening at all.
    CHECK_FALSE(waitUntil(
        [&] {
            return daemon.onDaemon([&daemon] {
                auto* root = daemon.host->model().window(daemon.host->windowId())->activeTab()->rootPane();
                return root->ratio() != 0.3;
            });
        },
        /*attempts=*/20));

    ac->stop();
}

namespace
{
/// @return A daemon whose host resolves the profiles "main" (its default) and "ops", and whose handshake
///         offers Capability::ProfileByName besides what it offers by default -- what `contour daemon` serves
///         with profiles configured.
[[nodiscard]] std::unique_ptr<DaemonFixture> daemonWithProfiles()
{
    using vthost::testing::fakeProfile;
    auto services = vthost::NativeSessionServices {};
    services.handshake.capabilities.idents.push_back(
        std::to_underlying(vthost::proto::Capability::ProfileByName));
    return std::make_unique<DaemonFixture>(
        nullptr,
        std::move(services),
        1,
        std::make_shared<vthost::testing::FakeProfileCatalog>(
            std::map<std::string, vthost::ResolvedProfile> {
                { "main", fakeProfile("/bin/main-shell", vtbackend::LineCount(1000)) },
                { "ops", fakeProfile("/bin/ops-shell", vtbackend::LineCount(2000)) },
            },
            "main"));
}

} // namespace

TEST_CASE("a new tab with a profile asks the daemon for that profile", "[attach][controller][profiles]")
{
    auto const daemon = daemonWithProfiles();
    std::ignore = daemon->seedSession("first");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon->endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabCount() == 1);

    // The CreateNewTab action's profile, through the manager as the keybinding takes it.
    app.manager().createNewTab(win.id, std::string { "ops" });

    auto const profiles = [&] {
        return daemon->onDaemon([&] {
            auto names = std::vector<std::string> {};
            daemon->host->model().forEachTab([&](vtworkspace::Window&, vtworkspace::Tab& tab) {
                names.push_back(daemon->host->hostedSession(tab.rootPane()->session())->profileName());
            });
            return names;
        });
    };
    REQUIRE(waitUntil([&] { return profiles().size() == 2; }));
    // Not the host's default ("main"): the name the tab asked for. The seeded tab names none.
    CHECK(profiles().back() == "ops");

    ac->stop();
}

// B3-Qt: the client authors a tab on the daemon; the daemon honors it and
// re-pushes its layout, which the incremental reconciler realizes as a new local
// tab — closing the create loop over a real attach connection.
TEST_CASE("attach authors a tab on the daemon and reconciles it locally", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && ac->settledLayout() == nullptr; ++i)
        std::this_thread::sleep_for(5ms);
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    REQUIRE(app.manager().model().window(win.id)->tabCount() == 1);

    // A GUI "new tab" in attach mode routes to the daemon (requestRemoteTab ->
    // requestCreateTab) instead of creating a local tab; wait for the two-tab
    // layout to arrive.
    app.manager().createNewTab(win.id);
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->settledLayout(); l && l->tabs.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(ac->settledLayout()->tabs.size() == 2);

    // Reconcile: the daemon's new tab appears locally (the first is left untouched).
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);
    CHECK(app.manager().model().window(win.id)->tabCount() == 2);

    ac->stop();
}

namespace
{
/// Writes a configuration file naming @p profile with @p terminalId, as a user saving it would.
void writeConfigWithTerminalId(std::filesystem::path const& file,
                               std::string const& profile,
                               std::string_view terminalId)
{
    contour::test::writeConfigFile(
        file,
        contour::test::profileConfigYaml(profile, {}, std::format("        terminal_id: {}\n", terminalId)));
}
} // namespace

// A session the client creates takes the client's profile from the configuration the GUI holds in
// memory -- what a reload updates, and what a local new tab is built from -- not from the file as saved.
// The sessions that already exist keep theirs.
TEST_CASE("a session the client creates gets the profile the GUI holds in memory", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto testApp = contour::test::ConfiguredTestApp {};
    auto& app = testApp.app();
    auto const profileName = app.profileName();
    auto const setInMemory = [&](vtbackend::VTType id) {
        REQUIRE(app.config().findProfile(profileName) != nullptr);
        app.config().findProfile(profileName)->terminalId.value() = id;
    };
    // The file says something else throughout, so a source that read it would be caught.
    writeConfigWithTerminalId(app.config().configFile, profileName, "VT320");
    setInMemory(vtbackend::VTType::VT420);

    auto timeline = TestTimeline {};
    auto const controllerOwned =
        makeNativeController(daemon.endpoint(), timeline, [&app] { return app.currentEmulationSettings(); });
    auto& controller = *controllerOwned;
    requireConnected(controller);

    auto const tabCount = [&] {
        return daemon.onDaemon(
            [&] { return daemon.host->model().window(daemon.host->windowId())->tabCount(); });
    };
    auto const terminalIdOfTab = [&](int index) {
        return daemon.onDaemon([&] {
            auto const* window = daemon.host->model().window(daemon.host->windowId());
            return daemon.host->terminal(window->tabAt(index)->rootPane()->session())->terminalId();
        });
    };

    controller.requestCreateTab(nullptr, {});
    REQUIRE(waitUntil([&] { return tabCount() == 2; }));
    CHECK(terminalIdOfTab(1) == vtbackend::VTType::VT420);

    // The configuration in memory changes, as a reload changes it; the file does not.
    setInMemory(vtbackend::VTType::VT220);
    controller.requestCreateTab(nullptr, {});
    REQUIRE(waitUntil([&] { return tabCount() == 3; }));
    CHECK(terminalIdOfTab(2) == vtbackend::VTType::VT220);

    // The file changes and nothing reloads it: the next tab still follows the configuration in memory.
    writeConfigWithTerminalId(app.config().configFile, profileName, "VT340");
    controller.requestCreateTab(nullptr, {});
    REQUIRE(waitUntil([&] { return tabCount() == 4; }));
    CHECK(terminalIdOfTab(3) == vtbackend::VTType::VT220);

    // The sessions that already existed keep what they were created with.
    CHECK(terminalIdOfTab(1) == vtbackend::VTType::VT420);

    controller.stop();
}

// B4: a second daemon window (authored via requestRemoteWindow) is reconciled into
// its OWN GUI window — the client maps one OS window per daemon window, each mirroring
// only that window's tabs, with no cross-window session bleed.
TEST_CASE("attach maps each daemon window onto its own GUI window", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("win one"); // the primary daemon window

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win1 { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && settledWindows(*ac).empty(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(settledWindows(*ac).size() == 1);

    // A GUI "new window" in attach mode authors it on the daemon (requestRemoteWindow
    // -> requestCreateWindow) rather than opening a stray local one; wait for the
    // daemon to grow and push the second window's layout.
    REQUIRE(ac->requestRemoteWindow({}));
    for (auto i = 0; i < 200 && settledWindows(*ac).size() < 2; ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(settledWindows(*ac).size() == 2);
    auto const ids = settledWindows(*ac);

    // The two windows carry distinct leaf sessions — reconciling them into separate
    // GUI windows must not bind one session into both.
    auto const sessionOf = [&](uint64_t window) {
        return ac->settledLayout(window)->tabs.front().root.session;
    };
    CHECK(sessionOf(ids[0]) != sessionOf(ids[1]));

    // Reconcile each daemon window into its own GUI window.
    contour::test::ScopedController const win2 { app.manager() };
    contour::remote::applyRemoteLayout(app.manager(), win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(app.manager(), win2.id, *ac, ids[1]);

    // Each GUI window mirrors exactly its daemon window's single tab...
    REQUIRE(app.manager().model().window(win1.id)->tabCount() == 1);
    REQUIRE(app.manager().model().window(win2.id)->tabCount() == 1);
    // ...bound to that window's own remote session (via the pane's pty).
    auto const boundSession = [&](vtworkspace::WindowId window) -> std::optional<uint64_t> {
        auto* tab = app.manager().model().window(window)->tabAt(0);
        auto const sessions = app.manager().sessionsOfTab(tab);
        if (sessions.empty())
            return std::nullopt;
        return ac->sessionForPty(&sessions.front()->terminal().device());
    };
    CHECK(boundSession(win1.id) == sessionOf(ids[0]));
    CHECK(boundSession(win2.id) == sessionOf(ids[1]));

    ac->stop();
}

// A "+" clicked in the SECOND attach-mode window must create its tab THERE. The request carried no
// window at all — SessionFactory::requestRemoteTab took no argument, proto::CreateTab had no field,
// and SessionHost::createTab used its own fixed window — so every window's "+" landed in the
// daemon's first one: the tab appeared in the wrong window and the window clicked in gained nothing.
TEST_CASE("attach authors a tab in the window the request came from", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("win one");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win1 { app.manager() };

    requireConnected(*ac);
    for (auto i = 0; i < 200 && settledWindows(*ac).empty(); ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(ac->requestRemoteWindow({}));
    for (auto i = 0; i < 200 && settledWindows(*ac).size() < 2; ++i)
        std::this_thread::sleep_for(5ms);
    REQUIRE(settledWindows(*ac).size() == 2);
    auto const ids = settledWindows(*ac);

    contour::test::ScopedController const win2 { app.manager() };
    contour::remote::applyRemoteLayout(app.manager(), win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(app.manager(), win2.id, *ac, ids[1]);
    REQUIRE(app.manager().model().window(win1.id)->tabCount() == 1);
    REQUIRE(app.manager().model().window(win2.id)->tabCount() == 1);

    // The "+" of the SECOND GUI window.
    app.manager().createNewTab(win2.id);
    for (auto i = 0; i < 200; ++i)
    {
        if (auto const l = ac->settledLayout(ids[1]); l && l->tabs.size() == 2)
            break;
        std::this_thread::sleep_for(5ms);
    }

    // The daemon grew the tab in the second window, and left the first alone.
    REQUIRE(ac->settledLayout(ids[1]) != nullptr);
    CHECK(ac->settledLayout(ids[1])->tabs.size() == 2);
    REQUIRE(ac->settledLayout(ids[0]) != nullptr);
    CHECK(ac->settledLayout(ids[0])->tabs.size() == 1);

    ac->stop();
}

namespace
{

/// @return Every TerminalSession the manager shows, in any window.
[[nodiscard]] std::set<contour::session::TerminalSession*> shownSessions(
    contour::session::TerminalSessionManager& manager)
{
    auto out = std::set<contour::session::TerminalSession*> {};
    for (auto const w: std::views::iota(0, manager.model().windowCount()))
        if (auto* const window = manager.model().windowAt(w))
            for (auto const t: std::views::iota(0, window->tabCount()))
                for (auto* session: manager.sessionsOfTab(window->tabAt(t)))
                    out.insert(session);
    return out;
}

/// @return The TerminalSession mirroring daemon session @p remote, or null.
[[nodiscard]] contour::session::TerminalSession* sessionMirroring(
    contour::session::TerminalSessionManager& manager,
    contour::remote::NativeController const& ac,
    uint64_t remote)
{
    for (auto* session: shownSessions(manager))
        if (ac.sessionForPty(&session->terminal().device()) == remote)
            return session;
    return nullptr;
}

/// @return The window of @p manager showing @p session, if any.
[[nodiscard]] std::optional<vtworkspace::WindowId> windowShowing(
    contour::session::TerminalSessionManager& manager, contour::session::TerminalSession const* session)
{
    auto const [window, tab, leaf] = manager.model().findSessionLeaf(session->modelSessionId());
    return window != nullptr ? std::optional { window->id() } : std::nullopt;
}

} // namespace

// A pane the daemon moves into another window is the same TerminalSession there: the pushed layouts are
// realized by moving it, never by closing it in one window and mirroring it anew in the other. The source
// window is reconciled first, as ContourGuiApp does (ascending daemon window ids), which is the order that
// would close the pane before the destination could take it.
TEST_CASE("a pane the daemon moves into another window keeps its TerminalSession",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("stays");
    auto const moved = daemon.seedSession("moved");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };

    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 2; }));
    REQUIRE(ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
    auto const ids = settledWindows(*ac);
    contour::test::ScopedController const win2 { manager };
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);
    REQUIRE(manager.model().window(win1.id)->tabCount() == 2);
    REQUIRE(manager.model().window(win2.id)->tabCount() == 1);

    auto* const movedSession = sessionMirroring(manager, *ac, moved.value);
    REQUIRE(movedSession != nullptr);
    auto const before = shownSessions(manager);
    auto const target = ac->settledLayout(ids[1])->tabs.front().root.session;

    REQUIRE(ac->moveSupport() == contour::session::MoveSupport::Supported);
    CHECK(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                  .sourceSessions = { moved.value },
                                  .target = vtworkspace::MoveTargetKind::Edge,
                                  .targetSession = target,
                                  .side = vtworkspace::SplitSide::Right,
                                  .index = 0,
                                  .request = 1 })
          == contour::session::RemoteRouting::Routed);
    // Authored on the daemon, not made here: nothing moved until its layouts arrive.
    CHECK(windowShowing(manager, movedSession) == win1.id);
    REQUIRE(waitUntil([&] {
        auto const source = ac->settledLayout(ids[0]);
        auto const destination = ac->settledLayout(ids[1]);
        return source && destination && source->tabs.size() == 1 && destination->tabs.front().root.isSplit();
    }));

    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);

    CHECK(manager.model().window(win1.id)->tabCount() == 1);
    REQUIRE(manager.model().window(win2.id)->tabCount() == 1);
    auto* const destinationTab = manager.model().window(win2.id)->tabAt(0);
    CHECK(destinationTab->paneCount() == 2);
    CHECK(std::ranges::contains(manager.sessionsOfTab(destinationTab), movedSession));
    // The very same object, and no other: nothing was spawned, nothing closed.
    CHECK(shownSessions(manager) == before);
    CHECK(ac->sessionForPty(&movedSession->terminal().device()) == moved.value);
    CHECK(daemon.onDaemon([&] { return daemon.host->terminal(moved) != nullptr; }));

    ac->stop();
}

// A pane the daemon moves into a window of its own: the window is pushed as one this client was never told
// of, and the GUI window opened for it holds the moved TerminalSession rather than a new mirror of it.
TEST_CASE("a pane the daemon moves into a new window is the same TerminalSession there",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("stays");
    auto const moved = daemon.seedSession("moved");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };

    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 2; }));
    contour::remote::applyRemoteLayout(manager, win1.id, *ac);
    auto* const movedSession = sessionMirroring(manager, *ac, moved.value);
    REQUIRE(movedSession != nullptr);
    auto const before = shownSessions(manager);

    CHECK(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                  .sourceSessions = { moved.value },
                                  .target = vtworkspace::MoveTargetKind::NewWindow,
                                  .targetSession = 0,
                                  .side = vtworkspace::SplitSide::Left,
                                  .index = 0,
                                  .request = 2 })
          == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil([&] {
        auto const ids = settledWindows(*ac);
        return ids.size() == 2 && ac->settledLayout(ids[1])->tabs.size() == 1
               && ac->settledLayout(ids[0])->tabs.size() == 1;
    }));
    auto const ids = settledWindows(*ac);

    // The window that lost the pane is reconciled before one is opened for the new window: it must not end
    // the pane the other window is about to take.
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    CHECK(shownSessions(manager) == before);

    contour::test::ScopedController const win2 { manager };
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);

    CHECK(manager.model().window(win1.id)->tabCount() == 1);
    REQUIRE(manager.model().window(win2.id)->tabCount() == 1);
    CHECK(manager.sessionsOfTab(manager.model().window(win2.id)->tabAt(0))
          == std::vector<contour::session::TerminalSession*> { movedSession });
    CHECK(shownSessions(manager) == before);
    CHECK(daemon.onDaemon([&] { return daemon.host->terminal(moved) != nullptr; }));

    ac->stop();
}

// The other order: the pane moves from the higher-id window into the lower, so the destination's frame comes
// first and its window takes the pane while the source's layout still names it.
TEST_CASE("a moved pane whose new window's layout comes first is adopted, never re-mirrored",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("target");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };

    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 1; }));
    REQUIRE(ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
    auto const ids = settledWindows(*ac);
    contour::test::ScopedController const win2 { manager };
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);
    // A second tab in the higher-id window, which is the one to move: the window keeps its first.
    manager.createNewTab(win2.id);
    REQUIRE(waitUntil([&] { return ac->settledLayout(ids[1])->tabs.size() == 2; }));
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);
    REQUIRE(manager.model().window(win2.id)->tabCount() == 2);
    auto const moved = ac->settledLayout(ids[1])->tabs.back().root.session;
    auto* const movedSession = sessionMirroring(manager, *ac, moved);
    REQUIRE(movedSession != nullptr);
    auto const before = shownSessions(manager);
    auto const target = ac->settledLayout(ids[0])->tabs.front().root.session;

    daemon.holdAfterLayoutOf(ids[0]);
    REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                    .sourceSessions = { moved },
                                    .target = vtworkspace::MoveTargetKind::Edge,
                                    .targetSession = target,
                                    .side = vtworkspace::SplitSide::Bottom,
                                    .index = 0,
                                    .request = 4 })
            == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil([&] { return ac->layout(ids[0])->tabs.front().root.isSplit(); }, 400, GuiEvents::Held));
    REQUIRE(ac->settledLayout(ids[1])->tabs.size() == 2);

    // The destination's frame is in and its end is not: the reconcile sees the push's predecessor only.
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    CHECK(windowShowing(manager, movedSession) == win2.id);

    // The push's end is in: the reconcile sees both windows of it.
    daemon.releaseHeld();
    REQUIRE(waitUntil([&] { return ac->settledLayout(ids[1])->tabs.size() == 1; }));
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);
    drainGuiEvents();

    CHECK(shownSessions(manager) == before);
    CHECK(windowShowing(manager, movedSession) == win1.id);
    CHECK(manager.model().window(win1.id)->tabAt(0)->paneCount() == 2);
    CHECK(manager.model().window(win2.id)->tabCount() == 1);

    ac->stop();
}

namespace
{
/// What a daemon offers when it predates Capability::LayoutBatch: everything a daemon of today does, but
/// that.
[[nodiscard]] vthost::NativeSessionServices servicesWithoutLayoutBatch()
{
    auto services = vthost::NativeSessionServices {};
    std::erase(services.handshake.capabilities.idents,
               std::to_underlying(vthost::proto::Capability::LayoutBatch));
    return services;
}

/// A daemon with two windows, the second showing one pane, and a client showing both: the pane @c moved, in
/// the first window, is what each test moves beside @c target in the second.
struct TwoWindowMove
{
    DaemonFixture daemon;
    vtworkspace::SessionId moved;
    TestTimeline timeline;
    contour::remote::NativeController* ac = nullptr;
    contour::test::TestApp app;
    contour::session::TerminalSessionManager& manager;
    contour::test::ScopedController win1;
    std::vector<uint64_t> ids;
    std::optional<contour::test::ScopedController> win2;
    contour::session::TerminalSession* movedSession = nullptr;
    std::set<contour::session::TerminalSession*> before;
    uint64_t target = 0;
    contour::test::RecordingAnnouncer* said2 = nullptr; ///< What win2's screen reader was told.

    explicit TwoWindowMove(vthost::NativeSessionServices services = {}):
        daemon({}, std::move(services)),
        moved(seed(daemon)),
        app(makeOwned()),
        manager(app.manager()),
        win1(manager)
    {
        requireConnected(*ac);
        REQUIRE(waitUntil(
            [&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 2; }));
        REQUIRE(ac->requestRemoteWindow({}));
        REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
        ids = settledWindows(*ac);
        app.app().config().accessibilityAnnouncements = true;
        win2.emplace(manager, recordInto(said2));
        contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
        contour::remote::applyRemoteLayout(manager, win2->id, *ac, ids[1]);
        movedSession = sessionMirroring(manager, *ac, moved.value);
        REQUIRE(movedSession != nullptr);
        before = shownSessions(manager);
        target = ac->settledLayout(ids[1])->tabs.front().root.session;
    }

    ~TwoWindowMove() { ac->stop(); }
    TwoWindowMove(TwoWindowMove const&) = delete;
    TwoWindowMove& operator=(TwoWindowMove const&) = delete;
    TwoWindowMove(TwoWindowMove&&) = delete;
    TwoWindowMove& operator=(TwoWindowMove&&) = delete;

    /// Moves @c moved beside @c target while the client is let have the first window's layout of the push
    /// only, and reconciles that window: the reconcile reads the push before, so the pane stays where it is.
    void moveHoldingTheDestination()
    {
        daemon.holdAfterLayoutOf(ids[0]);
        REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                        .sourceSessions = { moved.value },
                                        .target = vtworkspace::MoveTargetKind::Edge,
                                        .targetSession = target,
                                        .side = vtworkspace::SplitSide::Right,
                                        .index = 0,
                                        .request = 5 })
                == contour::session::RemoteRouting::Routed);
        REQUIRE(waitUntil([&] { return ac->layout(ids[0])->tabs.size() == 1; }, 400, GuiEvents::Held));
        REQUIRE_FALSE(ac->settledLayout(ids[1])->tabs.front().root.isSplit());
        contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
        CHECK(windowShowing(manager, movedSession) == win1.id);
    }

  private:
    [[nodiscard]] static vtworkspace::SessionId seed(DaemonFixture& daemon)
    {
        std::ignore = daemon.seedSession("stays");
        return daemon.seedSession("moved");
    }

    [[nodiscard]] std::unique_ptr<contour::remote::NativeController> makeOwned()
    {
        auto owned = makeNativeController(daemon.endpoint(), timeline);
        ac = owned.get();
        return owned;
    }

  public:
    /// Builds a window's screen reader as one that records into @p said.
    [[nodiscard]] static contour::window::NoticeAnnouncerFactory recordInto(
        contour::test::RecordingAnnouncer*& said)
    {
        return [&said](std::function<QObject*()> const& /*anchor*/)
                   -> std::unique_ptr<contour::platform::Announcer> {
            auto announcer = std::make_unique<contour::test::RecordingAnnouncer>();
            said = announcer.get();
            return announcer;
        };
    }

    /// The moves whose pushes have ended, waited for until @p request is among them: what the reconcile takes
    /// first (ContourGuiApp::reconcileAttachWindows).
    [[nodiscard]] std::vector<uint64_t> landedMoves(uint64_t request) const
    {
        auto landed = std::vector<uint64_t> {};
        REQUIRE(waitUntil([&] {
            std::ranges::copy(ac->takeLandedMoves(), std::back_inserter(landed));
            return std::ranges::contains(landed, request);
        }));
        return landed;
    }

    /// What window @p said was told last.
    [[nodiscard]] static QString lastSaid(contour::test::RecordingAnnouncer const* said)
    {
        return said == nullptr || said->said.empty() ? QString {} : said->said.back().message;
    }
};
} // namespace

// The push's frames split across reads, and the GUI runs dry between them: with the daemon marking each
// push's end, the reconcile sees none of the push until the destination's layout is in too, and the
// destination then takes the same TerminalSession.
TEST_CASE("a moved pane whose new window's layout comes in a later read is adopted once the push has ended",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    f.moveHoldingTheDestination();
    drainGuiEvents();
    CHECK(shownSessions(f.manager) == f.before);
    CHECK(f.ac->settledLayout(f.ids[0])->tabs.size() == 2);

    f.daemon.releaseHeld();
    REQUIRE(waitUntil([&] { return f.ac->settledLayout(f.ids[1])->tabs.front().root.isSplit(); }));
    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
    contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, f.ids[1]);
    drainGuiEvents();

    // Required: a pane closed meanwhile would leave movedSession dangling.
    REQUIRE(shownSessions(f.manager) == f.before);
    CHECK(windowShowing(f.manager, f.movedSession) == f.win2->id);
    CHECK(f.manager.model().window(f.win2->id)->tabAt(0)->paneCount() == 2);
    CHECK(f.ac->sessionForPty(&f.movedSession->terminal().device()) == f.moved.value);
}

// A pane dragged out of a two-pane tab into a tab of its own: the pane that stays keeps the tab it was in --
// the same local tab, title and history -- and the dragged one is the one moved into the new tab.
TEST_CASE("a pane dragged out of its tab leaves the other pane in that tab",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const stays = daemon.seedSession("stays");
    auto const dragged = daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return tab->activePane()->session();
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil(
        [&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.front().root.isSplit(); }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    REQUIRE(manager.model().window(win.id)->tabCount() == 1);
    auto const tab = manager.model().window(win.id)->tabAt(0)->id();
    auto* const staysSession = sessionMirroring(manager, *ac, stays.value);
    auto* const draggedSession = sessionMirroring(manager, *ac, dragged.value);
    REQUIRE((staysSession != nullptr && draggedSession != nullptr));
    auto const before = shownSessions(manager);

    REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                    .sourceSessions = { dragged.value },
                                    .target = vtworkspace::MoveTargetKind::TabWell,
                                    .targetSession = stays.value,
                                    .side = vtworkspace::SplitSide::Left,
                                    .index = 1,
                                    .request = 6 })
            == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil([&] { return ac->settledLayout()->tabs.size() == 2; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);

    REQUIRE(manager.model().window(win.id)->tabCount() == 2);
    CHECK(manager.sessionsOfTab(manager.model().findTab(tab))
          == std::vector<contour::session::TerminalSession*> { staysSession });
    CHECK(manager.model().window(win.id)->tabAt(0)->id() == tab);
    CHECK(manager.sessionsOfTab(manager.model().window(win.id)->tabAt(1))
          == std::vector<contour::session::TerminalSession*> { draggedSession });
    CHECK(shownSessions(manager) == before);

    ac->stop();
}

// What the daemon ends its pushes with is what the GUI reconciles: the next push's first frame, in before the
// queued reconcile runs, must not show.
TEST_CASE("a reconcile sees the layouts as of the last batch end, not a push still arriving",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    REQUIRE(f.ac->settledLayout(f.ids[0]) != nullptr);
    auto const settledTabs = f.ac->settledLayout(f.ids[0])->tabs.size();
    REQUIRE(settledTabs == 2);

    f.daemon.holdAfterLayoutOf(f.ids[0]);
    REQUIRE(f.ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                      .sourceSessions = { f.moved.value },
                                      .target = vtworkspace::MoveTargetKind::Edge,
                                      .targetSession = f.target,
                                      .side = vtworkspace::SplitSide::Right,
                                      .index = 0,
                                      .request = 7 })
            == contour::session::RemoteRouting::Routed);
    // The first frame of the push is in -- the source window has lost the pane -- and its end is not.
    REQUIRE(waitUntil([&] { return f.ac->layout(f.ids[0])->tabs.size() == 1; }, 400, GuiEvents::Held));
    CHECK(f.ac->settledLayout(f.ids[0])->tabs.size() == settledTabs);

    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
    CHECK(f.manager.model().window(f.win1.id)->tabCount() == 2);
    CHECK(windowShowing(f.manager, f.movedSession) == f.win1.id);

    f.daemon.releaseHeld();
    CHECK(waitUntil([&] { return f.ac->settledLayout(f.ids[0])->tabs.size() == 1; }));
}

// A daemon window the first frame of a push adds is not given an OS window before the push has ended: it
// would stay blank until then, since applyRemoteLayout realizes the settled view only.
TEST_CASE("a window a push still arriving adds is reconciled once the push has ended",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    auto const reconciled = [&] {
        return std::ranges::to<std::vector>(
            contour::remote::windowsToReconcile(*f.ac)
            | std::views::transform(&contour::remote::ReconciledWindow::daemonWindow));
    };
    REQUIRE(reconciled() == f.ids);

    // Window ids count up: the new window is the next one.
    auto const added = f.ids.back() + 1;
    f.daemon.holdAfterLayoutOf(added);
    REQUIRE(f.ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return f.ac->windowIds().size() == 3; }, 400, GuiEvents::Held));
    CHECK(reconciled() == f.ids);

    f.daemon.releaseHeld();
    REQUIRE(waitUntil([&] { return reconciled().size() == 3; }));
    auto const all = reconciled();
    CHECK(all.back() == added);
    CHECK(contour::remote::windowsToReconcile(*f.ac).back().tabs == contour::remote::DaemonWindowTabs::Some);
}

// The GUI's own move, made by the daemon: once the push that makes it has ended, the reconcile places it, and
// the receiving window says so and takes focus, the moved pane focused in it. The calls are the ones
// ContourGuiApp::reconcileAttachWindows makes, in its order.
TEST_CASE("a pane moved beside another window's pane by the daemon is announced there, and focused",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    auto const drag = f.manager.paneDragOf(*f.movedSession);
    REQUIRE(drag.has_value());
    REQUIRE(drag->host.kind == contour::window::HostKind::Daemon);
    auto* const targetSession = sessionMirroring(f.manager, *f.ac, f.target);
    REQUIRE(targetSession != nullptr);

    // Emitted on the reactor thread: recorded there, read here.
    auto refusals = std::atomic<int> { 0 };
    auto const refusalSeen = QObject::connect(
        f.ac,
        &contour::remote::NativeController::moveRefused,
        f.ac,
        [&refusals](int, int, quint64) { ++refusals; },
        Qt::DirectConnection);
    REQUIRE(f.manager.moveNode(*drag,
                               vtworkspace::EdgeTarget { .pane = targetSession->modelSessionId(),
                                                         .side = vtworkspace::SplitSide::Right },
                               nullptr,
                               {}));
    REQUIRE((f.manager.livePendingRemoteMove() != nullptr));
    // Nothing is said before the daemon made it.
    CHECK(TwoWindowMove::lastSaid(f.said2).isEmpty());

    // The manager numbers its requests from 1.
    f.manager.noteRemoteMovesLanded(f.landedMoves(1));
    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
    contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, f.ids[1]);
    f.manager.completeRemoteMoves(contour::session::TerminalSessionManager::ReconcileReach::Whole);

    CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
    CHECK(refusals == 0);
    QObject::disconnect(refusalSeen);
    CHECK(windowShowing(f.manager, f.movedSession) == f.win2->id);
    CHECK(TwoWindowMove::lastSaid(f.said2)
          == QStringLiteral("Pane moved to window %1").arg(f.win2->id.value));
    CHECK(f.manager.focusedWindow() == f.win2->id);
    CHECK((*f.win2)->activeSession() == f.movedSession);
    CHECK(shownSessions(f.manager) == f.before);
}

TEST_CASE("a pane the GUI moves into a new window over a daemon opens where it was dropped",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    auto const drag = f.manager.paneDragOf(*f.movedSession);
    REQUIRE(drag.has_value());
    auto* const screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);

    REQUIRE(f.manager.moveNode(*drag,
                               vtworkspace::NewWindowTarget {},
                               screen,
                               screen->availableGeometry().topLeft() + QPoint { 240, 160 }));
    auto const landed = f.landedMoves(1);
    f.manager.noteRemoteMovesLanded(landed);

    auto said3 = static_cast<contour::test::RecordingAnnouncer*>(nullptr);
    auto win3 = std::optional<contour::test::ScopedController> {};
    for (auto const& [daemonWindow, tabs, sessions]: contour::remote::windowsToReconcile(*f.ac))
    {
        if (daemonWindow == f.ids[0])
            contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, daemonWindow);
        else if (daemonWindow == f.ids[1])
            contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, daemonWindow);
        else
        {
            // The window the daemon made for the move: it opens where the pane was dropped, on that screen.
            CHECK(f.manager.stageRemoteMoveWindow(sessions) == screen);
            win3.emplace(f.manager, TwoWindowMove::recordInto(said3));
            REQUIRE((*win3)->initialPosition().has_value());
            CHECK((*win3)->initialPosition()->position == QPoint { 240, 160 });
            contour::remote::applyRemoteLayout(f.manager, win3->id, *f.ac, daemonWindow);
            f.manager.endWindowSpawn();
        }
    }
    REQUIRE(win3.has_value());
    f.manager.completeRemoteMoves(contour::session::TerminalSessionManager::ReconcileReach::Whole);

    CHECK(windowShowing(f.manager, f.movedSession) == win3->id);
    CHECK(TwoWindowMove::lastSaid(said3) == QStringLiteral("Pane moved to new window"));
    CHECK(f.manager.focusedWindow() == win3->id);
    CHECK((*win3)->activeSession() == f.movedSession);
    // A window the reconcile opens for anything else later opens wherever windows open.
    CHECK(f.manager.stageRemoteMoveWindow(std::array { f.moved.value }) == nullptr);
    CHECK(shownSessions(f.manager) == f.before);
}

// The daemon names one cause per push: two moves served before the push's end was sent share it, and a move
// that pushed nothing has none of its own. All the moves up to it are complete, and none after it is.
TEST_CASE("a push ended by a move completes every move asked before it", "[attach][controller][move][daemon]")
{
    auto inFlight =
        contour::remote::NativeController::MovesInFlight { { 3, 30 }, { 5, 50 }, { 8, 80 }, { 9, 90 } };

    CHECK(contour::remote::NativeController::takeCompletedMoves(inFlight, 0).empty());
    CHECK(inFlight.size() == 4);

    CHECK(contour::remote::NativeController::takeCompletedMoves(inFlight, 8)
          == std::vector<uint64_t> { 30, 50, 80 });
    CHECK(inFlight == contour::remote::NativeController::MovesInFlight { { 9, 90 } });

    // A cause naming no move in flight still completes the older ones, and keeps the newer.
    CHECK(contour::remote::NativeController::takeCompletedMoves(inFlight, 4).empty());
    CHECK(contour::remote::NativeController::takeCompletedMoves(inFlight, 9) == std::vector<uint64_t> { 90 });
    CHECK(inFlight.empty());
}

// A reconcile that has not opened the window the move asked for (it skips one whose OS window is still being
// spawned) places nothing of it: the move waits for the reconcile that does, instead of being dropped, which
// would open that window without its position and without announcing the move.
TEST_CASE("a landed move whose window is not open yet waits for the reconcile that opens it",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    auto const drag = f.manager.paneDragOf(*f.movedSession);
    REQUIRE(drag.has_value());
    auto* const screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);
    REQUIRE(f.manager.moveNode(*drag,
                               vtworkspace::NewWindowTarget {},
                               screen,
                               screen->availableGeometry().topLeft() + QPoint { 240, 160 }));
    f.manager.noteRemoteMovesLanded(f.landedMoves(1));

    // The source window loses the pane; the window it moved to is not opened.
    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
    f.manager.completeRemoteMoves(contour::session::TerminalSessionManager::ReconcileReach::Partial);
    CHECK((f.manager.livePendingRemoteMove() != nullptr));

    // Once the window is there, the move is placed.
    auto said3 = static_cast<contour::test::RecordingAnnouncer*>(nullptr);
    auto win3 = std::optional<contour::test::ScopedController> {};
    for (auto const& [daemonWindow, tabs, sessions]: contour::remote::windowsToReconcile(*f.ac))
        if (daemonWindow != f.ids[0] && daemonWindow != f.ids[1])
        {
            std::ignore = f.manager.stageRemoteMoveWindow(sessions);
            win3.emplace(f.manager, TwoWindowMove::recordInto(said3));
            contour::remote::applyRemoteLayout(f.manager, win3->id, *f.ac, daemonWindow);
            f.manager.endWindowSpawn();
        }
    REQUIRE(win3.has_value());
    f.manager.completeRemoteMoves(contour::session::TerminalSessionManager::ReconcileReach::Whole);
    CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
    CHECK(TwoWindowMove::lastSaid(said3) == QStringLiteral("Pane moved to new window"));
    CHECK(f.manager.focusedWindow() == win3->id);
}

TEST_CASE("a move the daemon refuses is no longer waited for", "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    auto stale = f.manager.paneDragOf(*f.movedSession);
    REQUIRE(stale.has_value());
    // A session the daemon does not have: what a drag whose shell exited meanwhile names.
    stale->sessions = { 9999 };
    // Emitted on the reactor thread: recorded there, read here.
    auto refused = std::atomic<uint64_t> { 0 };
    auto const refusalSeen = QObject::connect(
        f.ac,
        &contour::remote::NativeController::moveRefused,
        f.ac,
        [&refused](int, int, quint64 request) { refused = request; },
        Qt::DirectConnection);

    REQUIRE(f.manager.moveNode(*stale, vtworkspace::NewWindowTarget {}, nullptr, {}));
    REQUIRE((f.manager.livePendingRemoteMove() != nullptr));
    REQUIRE(waitUntil([&] { return refused != 0; }));
    QObject::disconnect(refusalSeen);
    auto const request = uint64_t { refused };
    // The refusal names the manager's own number for the request, which counts from 1.
    CHECK(request == 1);
    f.manager.remoteMoveRefused(request);
    CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
    CHECK(shownSessions(f.manager) == f.before);
}

// A move the host will not take keeps the host's answer as its reason: a drag naming two panes as one is not
// a pane the daemon could move, and nothing waits for it.
TEST_CASE("a move the host will not take is refused with the host's answer",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    auto forged = f.manager.paneDragOf(*f.movedSession);
    REQUIRE(forged.has_value());
    forged->sessions = { f.moved.value, f.target };

    auto const moved = f.manager.moveNode(*forged, vtworkspace::NewWindowTarget {}, nullptr, {});
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error() == contour::session::MoveFailure { contour::session::RemoteRouting::Refused });
    CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
    CHECK(shownSessions(f.manager) == f.before);
}

// A reconnect clears the layouts, and what the new connection pushes replaces them: until it has, a reconcile
// finds no layout and changes nothing -- it must not close a pane the same daemon still has -- and once it
// has, the panes are the same TerminalSessions where they were.
TEST_CASE("a pane survives a reconnect to the same daemon as the same TerminalSession",
          "[attach][controller][move][daemon][reconnect]")
{
    using vthost::client::ConnectionState;
    auto f = TwoWindowMove {};

    // The new connection's hello comes in, and its layouts are held back.
    f.daemon.holdNewConnectionsAtLayout();
    f.daemon.dropConnections();
    REQUIRE(waitUntil([&] { return f.ac->connectionState() == ConnectionState::Reconnecting; }));
    REQUIRE(waitUntil(
        [&] { return f.ac->connectionState() == ConnectionState::Connected && f.ac->windowIds().empty(); },
        1000));
    CHECK(f.ac->settledLayouts().empty());

    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
    contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, f.ids[1]);
    drainGuiEvents();
    REQUIRE(shownSessions(f.manager) == f.before);

    f.daemon.releaseHeld();
    REQUIRE(waitUntil([&] { return settledWindows(*f.ac).size() == 2; }));
    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
    contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, f.ids[1]);
    drainGuiEvents();
    REQUIRE(shownSessions(f.manager) == f.before);
    CHECK(windowShowing(f.manager, f.movedSession) == f.win1.id);
    CHECK(f.ac->sessionForPty(&f.movedSession->terminal().device()) == f.moved.value);
}

// The reactor clears what the GUI reads -- the settled layouts, the landed moves, the tab pairings -- as a
// reconnect is accepted, while the GUI thread reconciles against them: under the controller's lock, so a
// ThreadSanitizer run of this sees no race.
TEST_CASE("a reconnect clears the layouts while the GUI thread reconciles against them",
          "[attach][controller][move][daemon][reconnect][threading]")
{
    using vthost::client::ConnectionState;
    constexpr auto Reconnects = 3;
    auto f = TwoWindowMove {};
    auto const wireTab = f.ac->settledLayout(f.ids[0])->tabs.front().tabId;
    auto const localTab = f.manager.model().window(f.win1.id)->tabAt(0)->id();

    auto reconnected = std::atomic<int> { 0 };
    auto const dropper = std::jthread { [&] {
        for ([[maybe_unused]] auto const round: std::views::iota(0, Reconnects))
        {
            f.daemon.dropConnections();
            // Up again: a later drop then ends a connection the reconnect adopted.
            for ([[maybe_unused]] auto const poll: std::views::iota(0, 400))
            {
                if (f.ac->connectionState() == ConnectionState::Connected && !f.ac->settledLayouts().empty())
                    break;
                std::this_thread::sleep_for(5ms);
            }
            ++reconnected;
        }
    } };
    while (reconnected < Reconnects)
    {
        contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
        contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, f.ids[1]);
        std::ignore = f.ac->takeLandedMoves();
        std::ignore = f.ac->settledLayouts();
        f.ac->pairTab(localTab, wireTab);
        std::ignore = f.ac->tabPairedWith(wireTab);
        QCoreApplication::processEvents();
    }

    REQUIRE(waitUntil([&] { return settledWindows(*f.ac).size() == 2; }));
    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, f.ids[0]);
    contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, f.ids[1]);
    drainGuiEvents();
    CHECK(shownSessions(f.manager) == f.before);
}

// A move routed to the daemon is waited for; one no connection could send must say so, or the manager waits
// out its timeout for an answer that never comes, and the user is told nothing.
TEST_CASE("a move no connection can send is refused, and no longer waited for",
          "[attach][controller][move][daemon][reconnect]")
{
    using vthost::client::ConnectionState;
    auto f = TwoWindowMove {};
    auto refusal = std::optional<std::pair<int, quint64>> {};
    auto receiver = QObject {};
    QObject::connect(
        f.ac,
        &contour::remote::NativeController::moveRefused,
        &receiver,
        [&refusal](int reason, int, quint64 request) { refusal = std::pair { reason, request }; },
        Qt::QueuedConnection);
    auto const drag = f.manager.paneDragOf(*f.movedSession);
    REQUIRE(drag.has_value());

    f.daemon.refuseConnections();
    REQUIRE(waitUntil([&] { return f.ac->connectionState() == ConnectionState::Reconnecting; }));
    REQUIRE(f.manager.moveNode(*drag, vtworkspace::NewWindowTarget {}, nullptr, {}));
    REQUIRE((f.manager.livePendingRemoteMove() != nullptr));
    REQUIRE(waitUntil([&] { return refusal.has_value(); }));

    CHECK(refusal->first == contour::remote::NativeController::MoveNotSent);
    CHECK(refusal->second == 1); // the manager's number for the request
    f.manager.remoteMoveRefused(refusal->second);
    CHECK_FALSE((f.manager.livePendingRemoteMove() != nullptr));
    CHECK(shownSessions(f.manager) == f.before);
    f.daemon.serveAgain();
}

// A closed tab's id names nothing, and a restarted daemon hands out its tab ids from 1 again: a stale pairing
// must not outlive either.
TEST_CASE("the pairing of local and daemon tabs is dropped with the tab, and with the daemon run",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("pair");
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    auto const tab = manager.model().window(win.id)->tabAt(0)->id();
    CHECK(ac->pairedWireTab(tab).has_value());

    SECTION("a tab that no longer exists is forgotten by the next reconcile")
    {
        constexpr auto Gone = vtworkspace::TabId { 424242 };
        ac->pairTab(Gone, 7);
        REQUIRE(ac->pairedWireTab(Gone) == std::optional<uint64_t> { 7 });
        REQUIRE(ac->tabPairedWith(7) == Gone);
        contour::remote::forgetClosedTabPairings(manager, *ac);
        CHECK_FALSE(ac->pairedWireTab(Gone).has_value());
        CHECK_FALSE(ac->tabPairedWith(7).has_value());
        CHECK(ac->pairedWireTab(tab).has_value());
    }

    SECTION("a daemon tab is paired with one local tab at most")
    {
        auto const wireTab = *ac->pairedWireTab(tab);
        constexpr auto Other = vtworkspace::TabId { 434343 };
        ac->pairTab(Other, wireTab);
        CHECK(ac->tabPairedWith(wireTab) == Other);
        CHECK_FALSE(ac->pairedWireTab(tab).has_value());
    }

    SECTION("a restarted daemon starts with no pairing")
    {
        daemon.restartHost();
        REQUIRE(waitUntil([&] { return ac->pendingCount() >= 1; }, 3000));
        CHECK_FALSE(ac->pairedWireTab(tab).has_value());
    }

    ac->stop();
}

// Two panes of one tab trading places on the daemon trade places here, as the same two TerminalSessions.
TEST_CASE("a swap within one tab pushed by the daemon keeps both TerminalSessions",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const left = daemon.seedSession("left");
    auto const right = daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return tab->activePane()->session();
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil(
        [&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.front().root.isSplit(); }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    auto* const leftSession = sessionMirroring(manager, *ac, left.value);
    auto* const rightSession = sessionMirroring(manager, *ac, right.value);
    REQUIRE((leftSession != nullptr && rightSession != nullptr));
    auto const before = shownSessions(manager);

    REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                    .sourceSessions = { left.value },
                                    .target = vtworkspace::MoveTargetKind::Swap,
                                    .targetSession = right.value,
                                    .side = vtworkspace::SplitSide::Left,
                                    .index = 0,
                                    .request = 8 })
            == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil(
        [&] { return ac->settledLayout()->tabs.front().root.children.front().session == right.value; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    QCoreApplication::processEvents();

    auto const* const root = manager.model().window(win.id)->tabAt(0)->rootPane();
    REQUIRE((root->first() != nullptr && root->second() != nullptr));
    CHECK(root->first()->session() == rightSession->modelSessionId());
    CHECK(root->second()->session() == leftSession->modelSessionId());
    CHECK(shownSessions(manager) == before);

    ac->stop();
}

// A tab moved along its window's tab well on the daemon moves here, the same TerminalSession in it.
TEST_CASE("a tab reorder pushed by the daemon keeps the TerminalSessions",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const first = daemon.seedSession("first");
    std::ignore = daemon.seedSession("second");
    auto const third = daemon.seedSession("third");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win { manager };
    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 3; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    auto* const thirdSession = sessionMirroring(manager, *ac, third.value);
    REQUIRE(thirdSession != nullptr);
    auto const before = shownSessions(manager);

    // The last tab to the front of the well.
    REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Tab,
                                    .sourceSessions = { third.value },
                                    .target = vtworkspace::MoveTargetKind::TabWell,
                                    .targetSession = first.value,
                                    .side = vtworkspace::SplitSide::Left,
                                    .index = 0,
                                    .request = 9 })
            == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil([&] { return ac->settledLayout()->tabs.front().root.session == third.value; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    QCoreApplication::processEvents();

    auto* const window = manager.model().window(win.id);
    REQUIRE(window->tabCount() == 3);
    CHECK(manager.sessionsOfTab(window->tabAt(0))
          == std::vector<contour::session::TerminalSession*> { thirdSession });
    CHECK(shownSessions(manager) == before);

    ac->stop();
}

// The last pane of a window moved out: the emptied window closes, and the moved session lives on beside
// its new neighbour.
TEST_CASE("a window's last pane moved by the daemon closes the window and keeps the session",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const moved = daemon.seedSession("moved");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };

    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 1; }));
    REQUIRE(ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
    auto const ids = settledWindows(*ac);
    contour::test::ScopedController const win2 { manager };
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);
    auto* const movedSession = sessionMirroring(manager, *ac, moved.value);
    REQUIRE(movedSession != nullptr);
    auto const before = shownSessions(manager);
    auto const target = ac->settledLayout(ids[1])->tabs.front().root.session;

    CHECK(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                  .sourceSessions = { moved.value },
                                  .target = vtworkspace::MoveTargetKind::Edge,
                                  .targetSession = target,
                                  .side = vtworkspace::SplitSide::Bottom,
                                  .index = 0,
                                  .request = 10 })
          == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil([&] {
        auto const destination = ac->settledLayout(ids[1]);
        return destination && destination->tabs.front().root.isSplit();
    }));
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    contour::remote::applyRemoteLayout(manager, win2.id, *ac, ids[1]);

    CHECK(manager.model().window(win1.id) == nullptr);
    REQUIRE(manager.model().window(win2.id) != nullptr);
    CHECK(std::ranges::contains(manager.sessionsOfTab(manager.model().window(win2.id)->tabAt(0)),
                                movedSession));
    CHECK(shownSessions(manager) == before);
    // A later push for the emptied daemon window finds no local window to reconcile, and moves nothing back.
    contour::remote::applyRemoteLayout(manager, win1.id, *ac, ids[0]);
    CHECK(windowShowing(manager, movedSession) == win2.id);

    ac->stop();
}

namespace
{
/// @return Every leaf session of the daemon tree @p root, in tree order.
[[nodiscard]] std::vector<uint64_t> sessionsIn(vthost::proto::WirePane const& root)
{
    auto wire = std::vector<uint64_t> {};
    auto const collect = [&](this auto const& self, vthost::proto::WirePane const& node) -> void {
        if (!node.isSplit())
            wire.push_back(node.session);
        else
            for (auto const& child: node.children)
                self(child);
    };
    collect(root);
    return wire;
}

/// @return Whether the tabs of @p window show the daemon window @p daemonWindow's, as of the settled layouts:
///         tab by tab, the same sessions in the same tree order.
[[nodiscard]] bool mirrors(contour::session::TerminalSessionManager& manager,
                           contour::remote::NativeController const& ac,
                           vtworkspace::WindowId window,
                           uint64_t daemonWindow)
{
    auto const layout = ac.settledLayout(daemonWindow);
    auto const* const local = manager.model().window(window);
    if (layout == nullptr || local == nullptr || std::cmp_not_equal(local->tabCount(), layout->tabs.size()))
        return false;
    for (auto const [index, wireTab]: core::views::enumerate(layout->tabs))
    {
        auto const wire = sessionsIn(wireTab.root);
        auto const shown = std::ranges::to<std::vector>(
            local->tabAt(static_cast<int>(index))->leafSessions()
            | std::views::transform([&](vtworkspace::SessionId session) {
                  auto* const terminal = manager.sessionForId(session);
                  return terminal != nullptr ? ac.sessionForPty(&terminal->terminal().device()).value_or(0)
                                             : 0;
              }));
        if (shown != wire)
            return false;
    }
    return true;
}
} // namespace

// Two moves served before the GUI reconciles: the settled layouts hold both, and one reconcile of every
// window brings the local tabs to them -- the second move undoing where the first left a pane, and every pane
// the same TerminalSession throughout.
TEST_CASE("a push compounding two moves is realized whole, as the same TerminalSessions",
          "[attach][controller][move][daemon]")
{
    auto f = TwoWindowMove {};
    auto const stays =
        sessionMirroring(f.manager, *f.ac, f.ac->settledLayout(f.ids[0])->tabs.front().root.session);
    REQUIRE(stays != nullptr);

    // The pane of the first window's second tab beside the second window's pane; then the first tab's pane
    // trades places with that one.
    REQUIRE(f.ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                      .sourceSessions = { f.moved.value },
                                      .target = vtworkspace::MoveTargetKind::Edge,
                                      .targetSession = f.target,
                                      .side = vtworkspace::SplitSide::Right,
                                      .index = 0,
                                      .request = 21 })
            == contour::session::RemoteRouting::Routed);
    REQUIRE(f.ac->requestRemoteMove(
                { .source = vtworkspace::MoveSourceKind::Pane,
                  .sourceSessions = { f.ac->hostSessionIdOf(stays->terminal().device()).value() },
                  .target = vtworkspace::MoveTargetKind::Swap,
                  .targetSession = f.target,
                  .side = vtworkspace::SplitSide::Left,
                  .index = 0,
                  .request = 22 })
            == contour::session::RemoteRouting::Routed);
    std::ignore = f.landedMoves(22);

    auto const layouts = f.ac->settledLayouts();
    contour::remote::applyRemoteLayout(f.manager, f.win1.id, *f.ac, layouts, f.ids[0]);
    contour::remote::applyRemoteLayout(f.manager, f.win2->id, *f.ac, layouts, f.ids[1]);
    drainGuiEvents();

    REQUIRE(shownSessions(f.manager) == f.before);
    CHECK(mirrors(f.manager, *f.ac, f.win1.id, f.ids[0]));
    CHECK(mirrors(f.manager, *f.ac, f.win2->id, f.ids[1]));
    CHECK(windowShowing(f.manager, sessionMirroring(f.manager, *f.ac, f.target)) == f.win1.id);
    CHECK(windowShowing(f.manager, stays) == f.win2->id);
    CHECK(windowShowing(f.manager, f.movedSession) == f.win2->id);
}

// Two panes of two tabs trading places on the daemon trade places here: each tab keeps its other pane, and
// both moved panes are the same TerminalSessions in the other tab.
TEST_CASE("a swap across two tabs pushed by the daemon keeps every TerminalSession",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const splitActive = [&daemon] {
        return daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
            auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
            daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
            return tab->activePane()->session();
        });
    };
    std::ignore = daemon.seedSession("a1");
    auto const a2 = splitActive();
    std::ignore = daemon.seedSession("b1");
    auto const b2 = splitActive();

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win { manager };
    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 2; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    REQUIRE(mirrors(manager, *ac, win.id, settledWindows(*ac).front()));
    auto const tabs = std::array { manager.model().window(win.id)->tabAt(0)->id(),
                                   manager.model().window(win.id)->tabAt(1)->id() };
    auto const before = shownSessions(manager);

    REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                    .sourceSessions = { a2.value },
                                    .target = vtworkspace::MoveTargetKind::Swap,
                                    .targetSession = b2.value,
                                    .side = vtworkspace::SplitSide::Left,
                                    .index = 0,
                                    .request = 23 })
            == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil(
        [&] { return ac->settledLayout()->tabs.front().root.children.back().session == b2.value; }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    drainGuiEvents();

    CHECK(shownSessions(manager) == before);
    CHECK(mirrors(manager, *ac, win.id, settledWindows(*ac).front()));
    // The tabs stay the tabs they were: only the two panes moved.
    CHECK(manager.model().window(win.id)->tabAt(0)->id() == tabs[0]);
    CHECK(manager.model().window(win.id)->tabAt(1)->id() == tabs[1]);

    ac->stop();
}

namespace
{
/// A GUI process following exactly one daemon window -- what a second launch beside a first is given -- whose
/// screen reader records what it is told.
struct FollowingGui
{
    TestTimeline timeline;
    contour::remote::NativeController* controller = nullptr;
    contour::test::TestApp app;
    contour::test::RecordingAnnouncer* said = nullptr; ///< What the window's screen reader was told.
    std::optional<contour::test::ScopedController> window;
    uint64_t daemonWindow = 0; ///< The one daemon window it follows.

    FollowingGui(DaemonFixture const& daemon, uint64_t followed):
        app(makeOwned(daemon, followed)), daemonWindow(followed)
    {
        app.app().config().accessibilityAnnouncements = true;
        window.emplace(app.manager(), TwoWindowMove::recordInto(said));
        requireConnected(*controller);
        REQUIRE(waitUntil([&] { return controller->settledLayout(daemonWindow) != nullptr; }));
        reconcile();
    }

    ~FollowingGui() { controller->stop(); }
    FollowingGui(FollowingGui const&) = delete;
    FollowingGui& operator=(FollowingGui const&) = delete;
    FollowingGui(FollowingGui&&) = delete;
    FollowingGui& operator=(FollowingGui&&) = delete;

    [[nodiscard]] contour::session::TerminalSessionManager& manager() { return app.manager(); }
    [[nodiscard]] vtworkspace::WindowId windowId() const { return window->id; }

    /// Reconciles the window it follows, as of the last push's end.
    void reconcile() { contour::remote::applyRemoteLayout(manager(), windowId(), *controller, daemonWindow); }

    /// @return The local session showing the daemon's @p remote; null for none.
    [[nodiscard]] contour::session::TerminalSession* showing(uint64_t remote)
    {
        return sessionMirroring(manager(), *controller, remote);
    }

  private:
    [[nodiscard]] std::unique_ptr<contour::remote::NativeController> makeOwned(DaemonFixture const& daemon,
                                                                               uint64_t followed)
    {
        auto owned = std::make_unique<contour::remote::NativeController>(
            std::make_shared<vthost::TlsTransportFactory>(daemon.endpoint()),
            vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Windows,
                                          .windows = { followed } },
            std::string {},
            std::nullopt,
            contour::remote::NativeController::SessionSettingsSource {},
            std::nullopt,
            core::platform::defaultSteadyClock(),
            timeline.timeline);
        controller = owned.get();
        return owned;
    }
};

/// Where a cross-process drop puts the pane: beside the first pane of the receiving tab, or onto one of its
/// two panes.
struct CrossProcessDrop
{
    std::string_view name;
    vtworkspace::MoveTargetKind kind;
    vtworkspace::SplitSide side; ///< For an edge.
    std::size_t onto;            ///< Which pane of the receiving tab, by tree order.
};

constexpr auto CrossProcessDrops = std::array {
    CrossProcessDrop { .name = "left",
                       .kind = vtworkspace::MoveTargetKind::Edge,
                       .side = vtworkspace::SplitSide::Left,
                       .onto = 0 },
    CrossProcessDrop { .name = "right",
                       .kind = vtworkspace::MoveTargetKind::Edge,
                       .side = vtworkspace::SplitSide::Right,
                       .onto = 0 },
    CrossProcessDrop { .name = "top",
                       .kind = vtworkspace::MoveTargetKind::Edge,
                       .side = vtworkspace::SplitSide::Top,
                       .onto = 0 },
    CrossProcessDrop { .name = "bottom",
                       .kind = vtworkspace::MoveTargetKind::Edge,
                       .side = vtworkspace::SplitSide::Bottom,
                       .onto = 0 },
    CrossProcessDrop { .name = "swap with the first child",
                       .kind = vtworkspace::MoveTargetKind::Swap,
                       .side = vtworkspace::SplitSide::Left,
                       .onto = 0 },
    CrossProcessDrop { .name = "swap with the second child",
                       .kind = vtworkspace::MoveTargetKind::Swap,
                       .side = vtworkspace::SplitSide::Left,
                       .onto = 1 },
};
} // namespace

// A pane dragged out of one GUI process into another's window, each following only its own daemon window: the
// receiving GUI never mirrored the pane, so it realizes it into whichever side of the tree the daemon grafted
// it, and the drop is announced and focused there. The GUI it left shows it no more. Every side, and a swap
// with either child, since a pane the daemon puts FIRST in a split is the shape a realizer that only adds
// second children never reproduces.
TEST_CASE("a pane moved into another GUI process's window appears there, on every side",
          "[attach][controller][move][daemon]")
{
    for (auto const& drop: CrossProcessDrops)
        DYNAMIC_SECTION(drop.name)
        {
            auto daemon = DaemonFixture {};
            auto const first = daemon.seedSession("first");
            auto const second = daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
                auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
                daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
                return tab->activePane()->session();
            });
            auto const receiving = daemon.onDaemon([&daemon] { return daemon.host->windowId().value; });
            // The pane's window keeps a tab of its own once the pane left: it stays open there.
            auto const [sending, moved] =
                daemon.onDaemon([&daemon]() -> std::pair<uint64_t, vtworkspace::SessionId> {
                    auto* window = daemon.host->createWindow();
                    auto const pane = window->tabAt(0)->rootPane()->session();
                    std::ignore = daemon.host->createTab({}, pane);
                    return { window->id().value, pane };
                });
            auto const targets = std::array { first.value, second.value };

            auto from = FollowingGui { daemon, sending };
            auto into = FollowingGui { daemon, receiving };
            REQUIRE(mirrors(from.manager(), *from.controller, from.windowId(), sending));
            REQUIRE(mirrors(into.manager(), *into.controller, into.windowId(), receiving));
            REQUIRE(from.showing(moved.value) != nullptr);
            REQUIRE(into.showing(moved.value) == nullptr);
            auto* const onto = into.showing(targets.at(drop.onto));
            REQUIRE(onto != nullptr);

            // The drag comes from the sending process; the drop is the receiving one's.
            auto const drag = from.manager().paneDragOf(*from.showing(moved.value));
            REQUIRE(drag.has_value());
            auto const target =
                drop.kind == vtworkspace::MoveTargetKind::Edge
                    ? vtworkspace::MoveTarget { vtworkspace::EdgeTarget { .pane = onto->modelSessionId(),
                                                                          .side = drop.side } }
                    : vtworkspace::MoveTarget { vtworkspace::SwapTarget { .pane = onto->modelSessionId() } };
            REQUIRE(into.manager().moveNode(*drag, target, nullptr, {}));
            REQUIRE((into.manager().livePendingRemoteMove() != nullptr));

            // The manager numbers its requests from 1.
            auto landed = std::vector<uint64_t> {};
            REQUIRE(waitUntil([&] {
                std::ranges::copy(into.controller->takeLandedMoves(), std::back_inserter(landed));
                return std::ranges::contains(landed, uint64_t { 1 });
            }));
            REQUIRE(waitUntil([&] {
                auto const layout = from.controller->settledLayout(sending);
                return layout != nullptr
                       && !std::ranges::any_of(layout->tabs, [&](vthost::proto::WireTab const& tab) {
                              return std::ranges::contains(sessionsIn(tab.root), moved.value);
                          });
            }));

            // As ContourGuiApp::reconcileAttachWindows does it, in each process.
            from.reconcile();
            into.manager().noteRemoteMovesLanded(landed);
            into.reconcile();
            into.manager().completeRemoteMoves(
                contour::session::TerminalSessionManager::ReconcileReach::Whole);
            drainGuiEvents();

            auto* const shown = into.showing(moved.value);
            REQUIRE(shown != nullptr);
            CHECK(windowShowing(into.manager(), shown) == into.windowId());
            CHECK(mirrors(into.manager(), *into.controller, into.windowId(), receiving));
            CHECK_FALSE((into.manager().livePendingRemoteMove() != nullptr));
            CHECK_FALSE(TwoWindowMove::lastSaid(into.said).isEmpty());
            CHECK(into.manager().focusedWindow() == into.windowId());
            CHECK((*into.window)->activeSession() == shown);
            CHECK(from.showing(moved.value) == nullptr);
            CHECK(daemon.onDaemon([&] { return daemon.host->terminal(moved) != nullptr; }));
        }
}

namespace
{
/// A drop of one pane of a two-pane tab [first | second] on an edge of the other, and the tree it leaves.
struct SiblingEdgeDrop
{
    std::string_view name;
    std::size_t moved;                ///< Which pane is dragged, by tree order.
    vtworkspace::SplitSide side;      ///< The edge of the other pane it is dropped on.
    vtworkspace::SplitState split;    ///< The axis the tab is split along after it.
    std::array<std::size_t, 2> order; ///< The panes, by their tree order before the drop, after it.
};

constexpr auto SiblingEdgeDrops = std::array {
    SiblingEdgeDrop { .name = "second onto the first's top",
                      .moved = 1,
                      .side = vtworkspace::SplitSide::Top,
                      .split = vtworkspace::SplitState::Horizontal,
                      .order = { 1, 0 } },
    SiblingEdgeDrop { .name = "second onto the first's bottom",
                      .moved = 1,
                      .side = vtworkspace::SplitSide::Bottom,
                      .split = vtworkspace::SplitState::Horizontal,
                      .order = { 0, 1 } },
    SiblingEdgeDrop { .name = "second onto the first's left",
                      .moved = 1,
                      .side = vtworkspace::SplitSide::Left,
                      .split = vtworkspace::SplitState::Vertical,
                      .order = { 1, 0 } },
    SiblingEdgeDrop { .name = "first onto the second's left",
                      .moved = 0,
                      .side = vtworkspace::SplitSide::Left,
                      .split = vtworkspace::SplitState::Vertical,
                      .order = { 0, 1 } },
    SiblingEdgeDrop { .name = "first onto the second's right",
                      .moved = 0,
                      .side = vtworkspace::SplitSide::Right,
                      .split = vtworkspace::SplitState::Vertical,
                      .order = { 1, 0 } },
    SiblingEdgeDrop { .name = "first onto the second's top",
                      .moved = 0,
                      .side = vtworkspace::SplitSide::Top,
                      .split = vtworkspace::SplitState::Horizontal,
                      .order = { 0, 1 } },
    SiblingEdgeDrop { .name = "first onto the second's bottom",
                      .moved = 0,
                      .side = vtworkspace::SplitSide::Bottom,
                      .split = vtworkspace::SplitState::Horizontal,
                      .order = { 1, 0 } },
};
} // namespace

// A pane of a side-by-side split dropped on an edge of its only sibling, through the daemon and the reconcile
// ContourGuiApp runs: the local tab is re-split as the daemon's, though for a drop that keeps the leaf order
// only the axis tells the two trees apart.
TEST_CASE("a pane dropped on an edge of its only sibling over a daemon re-splits the tab along that edge",
          "[attach][controller][move][daemon]")
{
    for (auto const& drop: SiblingEdgeDrops)
        DYNAMIC_SECTION(drop.name)
        {
            auto daemon = DaemonFixture {};
            auto const first = daemon.seedSession("first");
            auto const second = daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
                auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
                daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
                return tab->activePane()->session();
            });
            auto const followed = daemon.onDaemon([&daemon] { return daemon.host->windowId().value; });
            auto const panes = std::array { first.value, second.value };

            auto gui = FollowingGui { daemon, followed };
            REQUIRE(mirrors(gui.manager(), *gui.controller, gui.windowId(), followed));
            auto* const dragged = gui.showing(panes.at(drop.moved));
            auto* const onto = gui.showing(panes.at(1 - drop.moved));
            REQUIRE((dragged != nullptr && onto != nullptr));

            auto const drag = gui.manager().paneDragOf(*dragged);
            REQUIRE(drag.has_value());
            REQUIRE(gui.manager().moveNode(
                *drag,
                vtworkspace::EdgeTarget { .pane = onto->modelSessionId(), .side = drop.side },
                nullptr,
                {}));
            auto landed = std::vector<uint64_t> {};
            REQUIRE(waitUntil([&] {
                std::ranges::copy(gui.controller->takeLandedMoves(), std::back_inserter(landed));
                return std::ranges::contains(landed, uint64_t { 1 });
            }));
            gui.manager().noteRemoteMovesLanded(landed);
            gui.reconcile();
            gui.manager().completeRemoteMoves(
                contour::session::TerminalSessionManager::ReconcileReach::Whole);
            drainGuiEvents();

            auto const* const tab = gui.manager().model().window(gui.windowId())->activeTab();
            REQUIRE(tab != nullptr);
            REQUIRE(tab->paneCount() == 2);
            auto const* const root = tab->rootPane();
            CHECK(root->splitState() == drop.split);
            CHECK(root->first()->session() == gui.showing(panes.at(drop.order[0]))->modelSessionId());
            CHECK(root->second()->session() == gui.showing(panes.at(drop.order[1]))->modelSessionId());
            CHECK(tab->activePane()->session() == dragged->modelSessionId());
            CHECK(mirrors(gui.manager(), *gui.controller, gui.windowId(), followed));
        }
}

// "Toggle Split Orientation" in a daemon-hosted window is authored on the daemon, not flipped locally: the
// next layout push, whatever caused it, would otherwise put the daemon's old axis back.
TEST_CASE("a split orientation toggled over a daemon is kept by the next layout push",
          "[attach][controller][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const first = daemon.seedSession("first");
    auto const tabId = daemon.onDaemon([&daemon]() -> vtworkspace::TabId {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return tab->id();
    });
    auto const followed = daemon.onDaemon([&daemon] { return daemon.host->windowId().value; });
    auto const daemonAxis = [&] {
        return daemon.onDaemon([&] { return daemon.host->model().findTab(tabId)->rootPane()->splitState(); });
    };

    auto gui = FollowingGui { daemon, followed };
    auto* const acting = gui.showing(first.value);
    REQUIRE(acting != nullptr);
    auto const* const localTab = gui.manager().model().window(gui.windowId())->activeTab();
    REQUIRE(localTab != nullptr);
    REQUIRE(localTab->rootPane()->splitState() == vtworkspace::SplitState::Vertical);

    gui.manager().toggleActivePaneOrientation(acting);

    // The daemon takes the new axis, and it is not a local flip a push would undo.
    CHECK(waitUntil([&] { return daemonAxis() == vtworkspace::SplitState::Horizontal; }));
    // An unrelated change pushes the layout again.
    std::ignore = daemon.onDaemon([&] {
        daemon.host->model().setTabTitle(tabId, "pushed");
        return true;
    });
    REQUIRE(waitUntil([&] {
        auto const layout = gui.controller->settledLayout(followed);
        return layout != nullptr && !layout->tabs.empty() && layout->tabs.front().title == "pushed";
    }));
    gui.reconcile();
    CHECK(gui.manager().model().window(gui.windowId())->activeTab()->rootPane()->splitState()
          == vtworkspace::SplitState::Horizontal);
}

// Two moves in one push: a pane out of the keeping window's split into a new tab of the losing window, and
// the losing window's only tab into the keeping window. A pass over the keeping window, made first, takes the
// losing window's every tab; the losing window must stay open for its own pass, which takes the pane in. Made
// through the reconcile ContourGuiApp runs, over the windows in either order.
TEST_CASE("a window a pass empties stays open for the pass that fills it again",
          "[attach][controller][move][daemon]")
{
    for (auto const losingFirst: { true, false })
        DYNAMIC_SECTION((losingFirst ? "the window losing its tab has the lower id" : "it has the higher id"))
        {
            auto daemon = DaemonFixture {};
            struct Seeded
            {
                uint64_t losing;              ///< The window whose only tab moves out.
                uint64_t keeping;             ///< The window it moves into.
                vtworkspace::SessionId lone;  ///< The pane of the losing window's tab.
                vtworkspace::SessionId kept;  ///< The keeping window's pane that stays.
                vtworkspace::SessionId taken; ///< The keeping window's pane that moves into the losing one.
            };
            auto const seeded = daemon.onDaemon([&daemon, losingFirst]() -> Seeded {
                auto& host = *daemon.host;
                auto* const firstTab = host.createTab();
                auto* const second = host.createWindow();
                auto* const secondTab = second->tabAt(0);
                auto* const keepingTab = losingFirst ? secondTab : firstTab;
                auto* const losingTab = losingFirst ? firstTab : secondTab;
                host.splitActivePane(keepingTab->id(), vtworkspace::SplitState::Vertical, 0.5);
                auto const firstWindow = host.windowId().value;
                return Seeded { .losing = losingFirst ? firstWindow : second->id().value,
                                .keeping = losingFirst ? second->id().value : firstWindow,
                                .lone = losingTab->rootPane()->session(),
                                .kept = keepingTab->leafSessions().front(),
                                .taken = keepingTab->activePane()->session() };
            });

            auto timeline = TestTimeline {};
            auto acOwned = makeNativeController(daemon.endpoint(), timeline);
            auto* ac = acOwned.get();
            contour::test::TestApp app { std::move(acOwned) };
            auto& manager = app.manager();
            contour::test::ScopedController const losingWindow { manager };
            contour::test::ScopedController const keepingWindow { manager };
            requireConnected(*ac);
            REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));

            auto windows = contour::remote::AttachWindowMap { { seeded.losing, losingWindow.id },
                                                              { seeded.keeping, keepingWindow.id } };
            auto opened = 0;
            auto const reconcile = [&] {
                contour::remote::reconcileAttachWindows(
                    manager, *ac, ac->settledLayouts(), windows, [&](auto const&) { ++opened; });
            };
            reconcile();
            REQUIRE(mirrors(manager, *ac, losingWindow.id, seeded.losing));
            REQUIRE(mirrors(manager, *ac, keepingWindow.id, seeded.keeping));
            auto const before = shownSessions(manager);

            REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                            .sourceSessions = { seeded.taken.value },
                                            .target = vtworkspace::MoveTargetKind::TabWell,
                                            .targetSession = seeded.lone.value,
                                            .side = vtworkspace::SplitSide::Left,
                                            .index = 1,
                                            .request = 31 })
                    == contour::session::RemoteRouting::Routed);
            REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                            .sourceSessions = { seeded.lone.value },
                                            .target = vtworkspace::MoveTargetKind::TabWell,
                                            .targetSession = seeded.kept.value,
                                            .side = vtworkspace::SplitSide::Left,
                                            .index = 1,
                                            .request = 32 })
                    == contour::session::RemoteRouting::Routed);
            auto landed = std::vector<uint64_t> {};
            REQUIRE(waitUntil([&] {
                std::ranges::copy(ac->takeLandedMoves(), std::back_inserter(landed));
                return std::ranges::contains(landed, uint64_t { 32 });
            }));

            reconcile();
            drainGuiEvents();

            REQUIRE(shownSessions(manager) == before);
            REQUIRE(manager.model().window(losingWindow.id) != nullptr);
            CHECK(mirrors(manager, *ac, losingWindow.id, seeded.losing));
            CHECK(mirrors(manager, *ac, keepingWindow.id, seeded.keeping));
            CHECK(windows
                  == contour::remote::AttachWindowMap { { seeded.losing, losingWindow.id },
                                                        { seeded.keeping, keepingWindow.id } });
            CHECK(opened == 0);

            // Converged: the next reconcile changes nothing.
            reconcile();
            CHECK(shownSessions(manager) == before);
            CHECK(mirrors(manager, *ac, losingWindow.id, seeded.losing));
            CHECK(mirrors(manager, *ac, keepingWindow.id, seeded.keeping));
            ac->stop();
        }
}

// The last pane of a window moved out through the reconcile ContourGuiApp runs: the window closes once every
// window had its pass, and its mapping goes with it, so a daemon window that held tabs again would get an OS
// window of its own.
TEST_CASE("a window a move leaves with no daemon tab closes after the reconcile, and is unmapped",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    auto const moved = daemon.seedSession("moved");
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 1; }));
    REQUIRE(ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
    auto const ids = settledWindows(*ac);
    contour::test::ScopedController const win2 { manager };
    auto windows = contour::remote::AttachWindowMap { { ids[0], win1.id }, { ids[1], win2.id } };
    auto const reconcile = [&] {
        contour::remote::reconcileAttachWindows(
            manager, *ac, ac->settledLayouts(), windows, [](auto const&) {});
    };
    reconcile();
    auto* const movedSession = sessionMirroring(manager, *ac, moved.value);
    REQUIRE(movedSession != nullptr);
    auto const before = shownSessions(manager);

    REQUIRE(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                    .sourceSessions = { moved.value },
                                    .target = vtworkspace::MoveTargetKind::Edge,
                                    .targetSession = ac->settledLayout(ids[1])->tabs.front().root.session,
                                    .side = vtworkspace::SplitSide::Left,
                                    .index = 0,
                                    .request = 33 })
            == contour::session::RemoteRouting::Routed);
    auto landed = std::vector<uint64_t> {};
    REQUIRE(waitUntil([&] {
        std::ranges::copy(ac->takeLandedMoves(), std::back_inserter(landed));
        return std::ranges::contains(landed, uint64_t { 33 });
    }));
    reconcile();
    drainGuiEvents();

    CHECK(manager.model().window(win1.id) == nullptr);
    CHECK(windows == contour::remote::AttachWindowMap { { ids[1], win2.id } });
    CHECK(windowShowing(manager, movedSession) == win2.id);
    CHECK(shownSessions(manager) == before);
    ac->stop();
}

// A window its user closed keeps its mapping while its daemon window holds tabs, so it does not open again;
// once the daemon window holds none the mapping goes, and the daemon window filled again opens a window of
// its own instead of being shown in the closed one, which is to say nowhere.
TEST_CASE("a closed window's daemon window, emptied and filled again, gets a window of its own",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 1; }));
    REQUIRE(ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
    auto const ids = settledWindows(*ac);
    // The second daemon window was shown in a window its user has closed since: one the model no longer has.
    auto const closed = vtworkspace::WindowId { 4242 };
    REQUIRE(manager.model().window(closed) == nullptr);
    auto windows = contour::remote::AttachWindowMap { { ids[0], win1.id }, { ids[1], closed } };
    auto opened = std::vector<uint64_t> {};
    auto const reconcile = [&] {
        contour::remote::reconcileAttachWindows(
            manager, *ac, ac->settledLayouts(), windows, [&](auto const& window) {
                opened.push_back(window.daemonWindow);
            });
    };

    // Still holding its tab: the closed window is not opened again.
    reconcile();
    CHECK(opened.empty());
    CHECK(windows.contains(ids[1]));

    // Emptied on the daemon: the mapping goes.
    auto const lone = ac->settledLayout(ids[1])->tabs.front().root.session;
    daemon.onDaemon([&daemon, lone] {
        daemon.host->closeSession(vtworkspace::SessionId { lone });
        return 0;
    });
    REQUIRE(waitUntil([&] {
        auto const layout = ac->settledLayout(ids[1]);
        return layout == nullptr || layout->tabs.empty();
    }));
    reconcile();
    CHECK(opened.empty());
    CHECK_FALSE(windows.contains(ids[1]));

    // Filled again: it gets a window of its own.
    auto const refilled = daemon.onDaemon([&daemon, &ids] {
        auto* const window = daemon.host->model().window(vtworkspace::WindowId { ids[1] });
        if (window == nullptr)
            return false;
        std::ignore = daemon.host->model().createTab(window->id());
        return true;
    });
    REQUIRE(refilled);
    REQUIRE(waitUntil([&] {
        auto const layout = ac->settledLayout(ids[1]);
        return layout != nullptr && !layout->tabs.empty();
    }));
    reconcile();
    CHECK(opened == std::vector { ids[1] });
    ac->stop();
}

// A snapshot that lacks a daemon window (a reconnect settling one window per frame, or a partial one) says
// nothing about that window: only an explicit empty layout does. The window its user closed stays closed.
TEST_CASE("a closed window stays closed when a snapshot lacks its daemon window",
          "[attach][controller][move][daemon]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 1; }));
    REQUIRE(ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
    auto const ids = settledWindows(*ac);
    auto const closed = vtworkspace::WindowId { 4242 };
    REQUIRE(manager.model().window(closed) == nullptr);
    auto windows = contour::remote::AttachWindowMap { { ids[0], win1.id }, { ids[1], closed } };
    auto opened = std::vector<uint64_t> {};
    auto const reconcile = [&](contour::remote::LayoutSnapshot const& layouts) {
        contour::remote::reconcileAttachWindows(manager, *ac, layouts, windows, [&](auto const& window) {
            opened.push_back(window.daemonWindow);
        });
    };

    auto partial = ac->settledLayouts();
    partial.erase(ids[1]);
    reconcile(partial);
    CHECK(opened.empty());
    CHECK(windows.contains(ids[1]));

    reconcile(ac->settledLayouts());
    CHECK(opened.empty());
    CHECK(windows.contains(ids[1]));
    ac->stop();
}

// The same across a real reconnect to a daemon that settles one window per frame.
TEST_CASE("a closed window stays closed across a reconnect that settles one window per frame",
          "[attach][controller][move][compat][reconnect]")
{
    auto daemon = DaemonFixture { {}, servicesWithoutLayoutBatch() };
    std::ignore = daemon.seedSession("first");
    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win1 { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 1; }));
    REQUIRE(ac->requestRemoteWindow({}));
    REQUIRE(waitUntil([&] { return settledWindows(*ac).size() == 2; }));
    auto const ids = settledWindows(*ac);
    auto const closed = vtworkspace::WindowId { 4242 };
    REQUIRE(manager.model().window(closed) == nullptr);
    auto windows = contour::remote::AttachWindowMap { { ids[0], win1.id }, { ids[1], closed } };
    auto opened = std::vector<uint64_t> {};
    auto const reconcile = [&] {
        contour::remote::reconcileAttachWindows(
            manager, *ac, ac->settledLayouts(), windows, [&](auto const& window) {
                opened.push_back(window.daemonWindow);
            });
    };

    daemon.dropConnections();
    // Reconciled after every frame the new connection settles, until both windows are back.
    REQUIRE(waitUntil([&] {
        reconcile();
        return settledWindows(*ac).size() == 2;
    }));
    reconcile();
    CHECK(opened.empty());
    CHECK(windows.contains(ids[1]));
    ac->stop();
}

// A daemon that does not end its pushes (one predating Capability::LayoutBatch) is reconciled frame by frame,
// as before there were moves: the GUI asks it for none, and a pane its layout drops is closed right away.
TEST_CASE("a daemon that does not end its pushes is reconciled frame by frame, and moves nothing",
          "[attach][controller][move][compat]")
{
    auto daemon = DaemonFixture { {}, servicesWithoutLayoutBatch() };
    std::ignore = daemon.seedSession("first");
    auto const second = daemon.onDaemon([&daemon]() -> vtworkspace::SessionId {
        auto* tab = daemon.host->model().window(daemon.host->windowId())->activeTab();
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        return tab->activePane()->session();
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const win { manager };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->settledLayout() != nullptr; }));
    CHECK(ac->moveSupport() == contour::session::MoveSupport::Unsupported);
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    REQUIRE(manager.model().window(win.id)->tabAt(0)->paneCount() == 2);

    daemon.onDaemon([&daemon, second] {
        daemon.host->handleSessionExit(second);
        return 0;
    });
    // Settled with the frame itself: no push end is waited for.
    REQUIRE(waitUntil([&] { return !ac->settledLayout()->tabs.front().root.isSplit(); }));
    contour::remote::applyRemoteLayout(manager, win.id, *ac);
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return manager.model().window(win.id)->tabAt(0)->paneCount() == 1;
    }));
    CHECK(sessionMirroring(manager, *ac, second.value) == nullptr);

    ac->stop();
}

TEST_CASE("a move the daemon cannot take is refused, never made here", "[attach][controller][move][compat]")
{
    using vthost::proto::Capability;
    auto daemon = DaemonFixture {};
    auto const one = daemon.seedSession("one");
    auto const two = daemon.seedSession("two");
    daemon.whileDisconnected([&] {
        daemon.services.handshake.capabilities = vthost::proto::CapabilitySet::of(
            { Capability::ExtensionPermissions, Capability::InitialClientArea, Capability::AttachIntent });
    });

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp app { std::move(acOwned) };
    contour::test::ScopedController const win { app.manager() };
    requireConnected(*ac);
    REQUIRE(
        waitUntil([&] { return ac->settledLayout() != nullptr && ac->settledLayout()->tabs.size() == 2; }));
    contour::remote::applyRemoteLayout(app.manager(), win.id, *ac);

    CHECK(ac->moveSupport() == contour::session::MoveSupport::Unsupported);
    CHECK(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                  .sourceSessions = { one.value },
                                  .target = vtworkspace::MoveTargetKind::Swap,
                                  .targetSession = two.value,
                                  .side = vtworkspace::SplitSide::Left,
                                  .index = 0,
                                  .request = 11 })
          == contour::session::RemoteRouting::Refused);
    CHECK(ac->daemonInstance() == daemon.instance);
    auto* const first = sessionMirroring(app.manager(), *ac, one.value);
    REQUIRE(first != nullptr);
    CHECK(ac->hostSessionIdOf(first->terminal().device()) == one.value);

    ac->stop();
}

TEST_CASE("a move names what it moves, or it is refused", "[attach][controller][move]")
{
    auto daemon = DaemonFixture {};
    auto const one = daemon.seedSession("one");
    auto const two = daemon.seedSession("two");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp const app { std::move(acOwned) };
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->moveSupport() == contour::session::MoveSupport::Supported; }));

    // No session, and a pane source naming two, name no pane the daemon could move.
    CHECK(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                  .sourceSessions = {},
                                  .target = vtworkspace::MoveTargetKind::NewWindow,
                                  .targetSession = 0,
                                  .side = vtworkspace::SplitSide::Left,
                                  .index = 0,
                                  .request = 12 })
          == contour::session::RemoteRouting::Refused);
    CHECK(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                  .sourceSessions = { one.value, two.value },
                                  .target = vtworkspace::MoveTargetKind::NewWindow,
                                  .targetSession = 0,
                                  .side = vtworkspace::SplitSide::Left,
                                  .index = 0,
                                  .request = 13 })
          == contour::session::RemoteRouting::Refused);

    ac->stop();
}

// The daemon's refusal reaches the GUI as a signal, which ContourGuiApp turns into a toast.
TEST_CASE("a move the daemon refuses is announced with its reason", "[attach][controller][move]")
{
    auto daemon = DaemonFixture {};
    auto const one = daemon.seedSession("one");

    auto timeline = TestTimeline {};
    auto acOwned = makeNativeController(daemon.endpoint(), timeline);
    auto* ac = acOwned.get();
    contour::test::TestApp const app { std::move(acOwned) };
    auto reasons = std::vector<int> {};
    // Emitted on the reactor thread: queued to this one, as ContourGuiApp connects it, so waitUntil's event
    // pumping delivers it here.
    auto receiver = QObject {};
    QObject::connect(
        ac,
        &contour::remote::NativeController::moveRefused,
        &receiver,
        [&](int reason) { reasons.push_back(reason); },
        Qt::QueuedConnection);
    requireConnected(*ac);
    REQUIRE(waitUntil([&] { return ac->moveSupport() == contour::session::MoveSupport::Supported; }));

    // A swap onto a session the daemon does not have.
    CHECK(ac->requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                  .sourceSessions = { one.value },
                                  .target = vtworkspace::MoveTargetKind::Swap,
                                  .targetSession = one.value + 1000,
                                  .side = vtworkspace::SplitSide::Left,
                                  .index = 0,
                                  .request = 14 })
          == contour::session::RemoteRouting::Routed);
    REQUIRE(waitUntil([&] { return !reasons.empty(); }));
    CHECK(reasons == std::vector<int> { 0 }); // vthost::NodeMoveError::UnknownSession

    ac->stop();
}

// Regression: closing a mirrored tab must not resurrect it. Before the fix,
// unbind() only forgot the binding, so the still-live remote session's next
// delta re-registered it as pending and re-adopted a fresh tab indefinitely.
TEST_CASE("a closed mirrored tab does not resurrect on later remote output", "[attach][controller]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("first line");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(controller.pendingCount() == 1);

    // Bind the pending remote session to a local tab, then close that tab.
    auto route = AttachRoute { controller };
    auto pty = route.router.createPty(std::nullopt);
    REQUIRE(pty != nullptr);
    auto pane = std::make_unique<MirrorPane>(route.router, std::move(pty));
    CHECK(pane->shows("first line"));
    CHECK(controller.pendingCount() == 0);
    pane.reset(); // the terminal destroyed its pty: the user closed the tab

    // The remote session lives on (nothing closed it) and keeps producing
    // output; a pre-fix controller would re-register it as pending here.
    daemon.onDaemon([&] {
        daemon.host->terminal(session)->writeToScreen("second line\r\n");
        return 0;
    });

    // Give the daemon's 20ms debounce ample time to push at least one more
    // delta, and confirm the closed session never came back as pending.
    auto resurrected = false;
    for ([[maybe_unused]] auto const iteration: std::views::iota(0, 50))
    {
        std::this_thread::sleep_for(10ms);
        if (controller.pendingCount() != 0)
        {
            resurrected = true;
            break;
        }
    }
    CHECK(!resurrected);
    CHECK(!controller.canCreateSession());

    controller.stop();
}

namespace
{

/// The connection states a controller went through, as connectionStateChanged() announced them.
struct StateLog
{
    explicit StateLog(contour::remote::NativeController& controller)
    {
        // Direct: it runs on the reactor, where the state changed, so no state is missed.
        QObject::connect(
            &controller, &contour::remote::NativeController::connectionStateChanged, [this, &controller] {
                auto const lock = std::lock_guard { mutex };
                states.push_back(controller.connectionState());
            });
        QObject::connect(&controller,
                         &contour::remote::NativeController::windowsLost,
                         [this](std::vector<uint64_t> const& daemonWindows) {
                             auto const lock = std::lock_guard { mutex };
                             lost.push_back(daemonWindows);
                         });
    }

    /// @return How many times @p state was entered.
    [[nodiscard]] std::size_t count(vthost::client::ConnectionState state) const
    {
        auto const lock = std::lock_guard { mutex };
        return static_cast<std::size_t>(std::ranges::count(states, state));
    }

    /// @return Every windowsLost() so far.
    [[nodiscard]] std::vector<std::vector<uint64_t>> windowsLost() const
    {
        auto const lock = std::lock_guard { mutex };
        return lost;
    }

    mutable std::mutex mutex;
    std::vector<vthost::client::ConnectionState> states;
    std::vector<std::vector<uint64_t>> lost;
};

} // namespace

TEST_CASE("a restarted listener re-snapshots the same sessions into the same panes",
          "[attach][controller][reconnect]")
{
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("before");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("before"));

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    CHECK(log.count(ConnectionState::Reconnecting) == 1);
    CHECK(controller.connectionState() == ConnectionState::Connected);
    CHECK(log.windowsLost().empty());
    CHECK(pane.shows("before"));

    // The same session reaches the same pane over the new connection.
    daemon.onDaemon([&] {
        daemon.host->terminal(session)->writeToScreen("\r\nafter");
        return 0;
    });
    CHECK(pane.shows("after"));
    CHECK_FALSE(pane.pty().isClosed());
    CHECK(controller.isBound(session.value));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a dark app's controller shows its first pane dark, never the host's light palette",
          "[attach][controller][colors]")
{
    using vthost::testing::FakeDualPaletteCatalog;
    auto daemon = DaemonFixture { {}, {}, 1, std::make_shared<FakeDualPaletteCatalog>() };
    std::ignore = daemon.seedSession("first");

    auto timeline = TestTimeline {};
    auto const controllerOwned =
        makeNativeController(daemon.endpoint(), timeline, {}, vtbackend::ColorPreference::Dark);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("first"));
    {
        auto const guard = std::lock_guard { *pane.terminal };
        CHECK(pane.terminal->colorPalette().defaultBackground == FakeDualPaletteCatalog::DarkBackground);
    }

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a preference stated while disconnected reaches the daemon once it answers again",
          "[attach][controller][reconnect][colors]")
{
    // The client states light, the connection drops, the OS goes dark while it is down, and the reconnect
    // must leave the mirror dark: the request made while down is refused, so the reconnect states it again.
    using contour::session::HostedOperation;
    using contour::session::HostedOperationRequest;
    using contour::session::HostedOperationRoute;
    using vthost::client::ConnectionState;
    using vthost::testing::FakeDualPaletteCatalog;
    auto daemon = DaemonFixture { {}, {}, 1, std::make_shared<FakeDualPaletteCatalog>() };
    std::ignore = daemon.seedSession("colors");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("colors"));
    auto const backgroundIsDark = [&] {
        auto const guard = std::lock_guard { *pane.terminal };
        return pane.terminal->colorPalette().defaultBackground == FakeDualPaletteCatalog::DarkBackground;
    };
    REQUIRE_FALSE(backgroundIsDark());

    CHECK(controller.requestHostedOperation(
              &pane.pty(), HostedOperationRequest { .operation = HostedOperation::UseLightColors })
          == HostedOperationRoute::RunOnHost);

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Reconnecting; }));
    CHECK(controller.requestHostedOperation(
              &pane.pty(), HostedOperationRequest { .operation = HostedOperation::UseDarkColors })
          == HostedOperationRoute::Unreachable);

    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }, 1000));
    CHECK(waitUntil(backgroundIsDark));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("keys typed while reconnecting are dropped", "[attach][controller][reconnect]")
{
    // Review focus 5: never replayed into whatever shell answers after the reconnect.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession("typing");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("typing"));

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Reconnecting; }));
    std::ignore = pane.pty().write("ls\r");
    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }, 1000));

    // What is typed now arrives; what was typed while down never does, not even behind it.
    std::ignore = pane.pty().write("pwd\r");
    CHECK(waitUntil([&] { return daemon.ptyInput(session).contains("pwd\r"); }));
    CHECK(daemon.ptyInput(session) == "pwd\r");

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a pane closed while reconnecting is closed on the daemon once it answers again",
          "[attach][controller][reconnect]")
{
    // A close is an intent that outlives the gap, unlike a keystroke: dropped, the shell would keep running
    // on the daemon, tombstoned here and so never shown by this client again.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    std::ignore = daemon.seedSession("closed");
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.pendingCount() == 2; }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    auto const closed = controller.sessionForPty(&pane.pty());
    REQUIRE(closed.has_value());
    REQUIRE(liveSessions() == 2);

    daemon.refuseConnections();
    REQUIRE(waitUntil([&] { return controller.connectionState() == ConnectionState::Reconnecting; }));
    route.router.requestRemoteClose(&pane.pty());
    // Long enough for the close to reach the reactor while no connection is up; nothing reached the daemon.
    std::this_thread::sleep_for(100ms);
    CHECK(controller.connectionState() == ConnectionState::Reconnecting);
    CHECK(liveSessions() == 2);

    daemon.serveAgain();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    CHECK(waitUntil([&] { return liveSessions() == 1; }));
    CHECK(daemon.onDaemon(
        [&] { return daemon.host->terminal(vtworkspace::SessionId { *closed }) == nullptr; }));

    controller.stop();
    pane.terminal.reset();
}

namespace contour::remote
{

/// Reaches the reactor-owned client of a controller (@see NativeController's friend declaration).
struct NativeControllerTestAccess
{
    /// Runs @p action on the reactor thread with the controller's current client, and waits for it.
    template <typename F>
    static void onReactor(contour::remote::NativeController& controller, F action)
    {
        auto done = std::promise<void> {};
        controller._reactor.post([&] {
            action(*controller._client);
            done.set_value();
        });
        done.get_future().wait();
    }

    /// Makes the reactor's run end with an exception, as a decode error escaping the root task would.
    static void killReactor(contour::remote::NativeController& controller)
    {
        controller._reactor.post([] { throw std::runtime_error("reactor died"); });
    }

    /// Ends the current client's connection and, in the same reactor turn, closes @p session's pane.
    static void closeWhileEnding(contour::remote::NativeController& controller, uint64_t session)
    {
        onReactor(controller, [&](auto& client) {
            client.detach();
            controller.closePaneOnReactor(session);
        });
    }
};

} // namespace contour::remote

TEST_CASE("a pane closed as its connection ends is closed once the same daemon answers again",
          "[attach][controller][reconnect]")
{
    // The close is issued with the client still current but its connection ending, so the client writes
    // nothing; the state change that would park it in _pendingCloses has not been delivered yet.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    std::ignore = daemon.seedSession("closed");
    auto const liveSessions = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->sessionCount(); });
    };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.pendingCount() == 2; }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    auto const closed = controller.sessionForPty(&pane.pty());
    REQUIRE(closed.has_value());
    REQUIRE(liveSessions() == 2);

    // The close lands on the reactor in the same turn as the detach, ahead of the state change it provokes.
    contour::remote::NativeControllerTestAccess::closeWhileEnding(controller, *closed);

    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 2000));
    CHECK(waitUntil([&] { return liveSessions() == 1; }));
    CHECK(daemon.onDaemon(
        [&] { return daemon.host->terminal(vtworkspace::SessionId { *closed }) == nullptr; }));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("the daemon's echo report reaches the pane's device and its observer, with no output after it",
          "[attach][controller][inputecho]")
{
    // sudo may turn echo off after its prompt printed: nothing follows, so the daemon's idle check tells the
    // client (FlushPolicy::inputEchoCheck, short here: it is a setting, not a sleep), and the controller
    // routes the report to the pane's device and wakes its owner (vtpty::Pty::setInputEchoObserver).
    auto services = vthost::NativeSessionServices {};
    services.flush.inputEchoCheck = std::chrono::milliseconds { 2 };
    auto daemon = DaemonFixture { {}, services };
    auto const session = daemon.seedSession("prompt");
    auto const setEchoOnDaemon = [&](vtpty::InputEcho echo) {
        daemon.onDaemon([&] {
            auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
            dynamic_cast<vtpty::MockPty&>(tapped.inner()).setInputEcho(echo);
            return 0;
        });
    };

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("prompt"));
    REQUIRE(controller.isBound(session.value));

    auto woken = std::atomic<int> { 0 };
    pane.pty().setInputEchoObserver([&woken] { ++woken; }); // runs on the reactor thread
    CHECK(pane.pty().inputEcho() == vtpty::InputEcho::Unknown);

    setEchoOnDaemon(vtpty::InputEcho::Hidden);
    REQUIRE(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Hidden; }));
    CHECK(woken.load() == 1);

    setEchoOnDaemon(vtpty::InputEcho::Echoed);
    REQUIRE(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Echoed; }));
    CHECK(woken.load() == 2);

    pane.pty().setInputEchoObserver({}); // before the counter goes
    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a pane's echo state is not trusted while the connection is down",
          "[attach][controller][inputecho][reconnect]")
{
    // Review M1: with typed input dropped and nothing reading, a stale Hidden would keep the lock and secure
    // input on. The pane reads Unknown while reconnecting; the reconnect's snapshot states the real one.
    auto services = vthost::NativeSessionServices {};
    services.flush.inputEchoCheck = std::chrono::milliseconds { 2 };
    auto daemon = DaemonFixture { {}, services };
    auto const session = daemon.seedSession("prompt");
    daemon.onDaemon([&] {
        auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
        dynamic_cast<vtpty::MockPty&>(tapped.inner()).setInputEcho(vtpty::InputEcho::Hidden);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("prompt"));
    REQUIRE(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Hidden; }));

    daemon.refuseConnections();
    CHECK(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Unknown; }));
    daemon.serveAgain();
    CHECK(waitUntil([&] { return pane.pty().inputEcho() == vtpty::InputEcho::Hidden; }, 3000));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a restarted daemon's new pane does not inherit the old run's echo state",
          "[attach][controller][inputecho][reconnect]")
{
    // Review M1: session ids count from 1 in every run of a daemon, so a new pane with the old id must not
    // be handed the old pane's Hidden -- the daemon says nothing of a PTY that reads Unknown.
    auto services = vthost::NativeSessionServices {};
    services.flush.inputEchoCheck = std::chrono::milliseconds { 2 };
    auto daemon = DaemonFixture { {}, services };
    auto const session = daemon.seedSession("old run");
    daemon.onDaemon([&] {
        auto& tapped = dynamic_cast<vthost::TappingPty&>(daemon.host->terminal(session)->device());
        dynamic_cast<vtpty::MockPty&>(tapped.inner()).setInputEcho(vtpty::InputEcho::Hidden);
        return 0;
    });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    auto route = AttachRoute { controller };
    auto old = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(old.shows("old run"));
    REQUIRE(waitUntil([&] { return old.pty().inputEcho() == vtpty::InputEcho::Hidden; }));

    daemon.restartHost();
    REQUIRE(waitUntil([&] { return controller.pendingCount() == 1; }, 3000));
    auto fresh = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    CHECK(fresh.pty().inputEcho() == vtpty::InputEcho::Unknown);

    controller.stop();
    fresh.terminal.reset();
    old.terminal.reset();
}

TEST_CASE("a reconnect takes the handshake of the daemon that answered", "[attach][controller][reconnect]")
{
    // The daemon may have been upgraded or downgraded meanwhile: what was agreed is the new connection's.
    using vthost::client::ConnectionState;
    using vthost::proto::Capability;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("upgraded");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(controller.negotiated().has_value());
    REQUIRE(controller.negotiated()->shared.contains(Capability::TabEdits));

    daemon.whileDisconnected([&] {
        daemon.services.handshake.capabilities =
            vthost::proto::CapabilitySet::of({ Capability::ExtensionPermissions,
                                               Capability::InitialClientArea,
                                               Capability::AttachIntent,
                                               Capability::Heartbeat });
    });
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    REQUIRE(controller.negotiated().has_value());
    CHECK_FALSE(controller.negotiated()->shared.contains(Capability::TabEdits));
    CHECK(controller.negotiated()->shared.contains(Capability::AttachIntent));

    controller.stop();
}

TEST_CASE("windows the daemon no longer has are reported lost", "[attach][controller][reconnect]")
{
    // Review focus 2: the daemon restarted, and none of the windows exists any more.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("old run");
    auto const window = daemon.onDaemon([&] { return daemon.host->windowId().value; });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    REQUIRE(pane.shows("old run"));

    // Queued, as the GUI connects it: the lost windows must survive the trip to the GUI thread.
    auto receiver = QObject {};
    auto queued = std::vector<uint64_t> {};
    QObject::connect(
        &controller,
        &contour::remote::NativeController::windowsLost,
        &receiver,
        [&queued](std::vector<uint64_t> const& daemonWindows) { queued = daemonWindows; },
        Qt::QueuedConnection);

    daemon.restartHost();
    REQUIRE(waitUntil([&] { return !log.windowsLost().empty(); }, 1000));
    CHECK(log.windowsLost() == std::vector<std::vector<uint64_t>> { { window } });
    CHECK(waitUntil([&] { return !queued.empty(); }));
    CHECK(queued == std::vector<uint64_t> { window });
    // The Resume found nothing to resume, fell back to a launch, and the new run's first window arrived:
    // its session awaits a pane of its own, rather than the old pane that had the same id.
    CHECK(controller.connectionState() == ConnectionState::Connected);
    CHECK(waitUntil([&] { return controller.pendingCount() == 1; }));
    CHECK(controller.layout().has_value());

    // The old pane is detached: what it is sent goes nowhere, least of all to the new session of its id.
    std::ignore = pane.pty().write("stale\r");
    auto const fresh = daemon.onDaemon([&] {
        return daemon.host->model().window(daemon.host->windowId())->activeTab()->rootPane()->session();
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(daemon.ptyInput(fresh).contains("stale"));

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("a client following every window still gets new windows after a reconnect",
          "[attach][controller][reconnect]")
{
    // A reconnect must not narrow the view: AllWindows comes back as AllWindows, not as Resume of the windows
    // it had, so a window another client creates afterwards still reaches it.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("first");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline); // AllWindows
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.windowIds().size() == 1; }));

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    REQUIRE(waitUntil([&] { return controller.windowIds().size() == 1; }));

    // Claimed by another client: a client following only listed windows would never see it.
    auto const otherClient = vthost::SessionStreamEvents {};
    auto const created =
        daemon.onDaemon([&] { return daemon.host->createWindow({}, &otherClient)->id().value; });
    CHECK(waitUntil([&] { return std::ranges::contains(controller.windowIds(), created); }));
    CHECK(log.windowsLost().empty());

    controller.stop();
}

TEST_CASE("a window gone from the same daemon is reported lost, the others kept",
          "[attach][controller][reconnect]")
{
    // Same instance: the ids still mean what they meant, and the lost windows are the ones not followed
    // again.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto const kept = daemon.onDaemon([&] { return daemon.host->windowId().value; });
    auto const gone = daemon.onDaemon([&] { return daemon.host->createWindow()->id().value; });

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.windowIds().size() == 2; }));

    // Closed on the daemon while this client was away: its only session ended.
    daemon.whileDisconnected([&] {
        auto* window = daemon.host->model().window(vtworkspace::WindowId { gone });
        daemon.host->handleSessionExit(window->tabAt(0)->rootPane()->session());
    });
    REQUIRE(waitUntil([&] { return !log.windowsLost().empty(); }, 1000));
    CHECK(log.windowsLost() == std::vector<std::vector<uint64_t>> { { gone } });
    CHECK(controller.connectionState() == ConnectionState::Connected);
    // Still followed (every window is), but with nothing in it: no OS window is opened for it.
    CHECK(controller.layout(kept).has_value());
    auto const emptied = controller.layout(gone);
    CHECK((!emptied || emptied->tabs.empty()));

    controller.stop();
}

TEST_CASE("a daemon that states no instance is judged by its window ids alone",
          "[attach][controller][reconnect]")
{
    // Instance 0 on either side is the old rule, mapped − followed: the restarted daemon's window 1 passes
    // for the one this client showed.
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture { {}, {}, /*instance=*/0 };
    std::ignore = daemon.seedSession("unnamed run");

    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const log = StateLog { controller };
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));

    daemon.restartHost();
    REQUIRE(waitUntil([&] { return log.count(ConnectionState::Connected) == 2; }, 1000));
    CHECK(waitUntil([&] { return controller.layout().has_value(); }));
    CHECK(log.windowsLost().empty());

    controller.stop();
}

TEST_CASE("stop() during a slow daemon start returns promptly", "[attach][controller][threading]")
{
    // The start runs on the transport's worker, not on the reactor stop() has to join: quitting while a
    // daemon is being started does not wait for it to serve.
    auto started = std::atomic<bool> { false };
    auto release = std::promise<void> {};
    auto const transport = std::make_shared<vthost::UnixTransportFactory>(
        "/nonexistent/contour/control", [&started, released = release.get_future().share()] {
            started = true;
            std::ignore = released.wait_for(10s);
            return std::expected<void, vthost::TransportError> {};
        });
    auto timeline = TestTimeline {};
    auto controller = contour::remote::NativeController {
        transport,
        vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Launch, .windows = {} },
        std::string {},
        std::nullopt,
        {},
        std::nullopt,
        core::platform::defaultSteadyClock(),
        timeline.timeline
    };
    controller.startConnecting();
    REQUIRE(waitUntil([&] { return started.load(); }));

    auto const began = std::chrono::steady_clock::now();
    controller.stop();
    auto const stoppedIn = std::chrono::steady_clock::now() - began;
    release.set_value();

    CHECK(stoppedIn < 1s);
    CHECK(controller.connectionState() == vthost::client::ConnectionState::Stopped);
}

TEST_CASE("NativeController shows the refusal text", "[contour][remote]")
{
    struct Row
    {
        std::string_view name;
        vthost::HandshakePolicy policy;
        vthost::proto::VersionRange daemonVersions;
    };
    auto const rows = std::array {
        Row { .name = "a daemon of other versions",
              .policy = { .versions = { .min = 6, .max = 8 } },
              .daemonVersions = { .min = 6, .max = 8 } },
        // The ranges overlap: the daemon refused for the token, and the text says so.
        Row { .name = "a daemon wanting another token",
              .policy = { .acceptedTokens = { { .token = "right" } } },
              .daemonVersions = vthost::proto::SupportedVersions },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto daemon = DaemonFixture { {}, vthost::NativeSessionServices { .handshake = row.policy } };
        std::ignore = daemon.seedSession("refused");

        auto timeline = TestTimeline {};
        auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
        auto& controller = *controllerOwned;
        auto const connected = controller.connectAndWait(10s);
        REQUIRE_FALSE(connected.has_value());
        // The refusal, naming the daemon it came from.
        CHECK(connected.error()
              == std::format("{} (tls://127.0.0.1:{})",
                             vthost::proto::describe(
                                 { .ours = vthost::proto::SupportedVersions, .theirs = row.daemonVersions },
                                 vthost::proto::PeerRole::Daemon),
                             daemon.port));
        controller.stop();
    }
}

TEST_CASE("NativeController.reconnect.afterARefusal", "[attach][controller][reconnect][manual]")
{
    using contour::session::ReconnectError;
    using vthost::client::ConnectionState;
    // A daemon that wants another token refuses the attach for good; once it accepts, a reconnect from the
    // footer reaches it on a new supervised run.
    auto daemon = DaemonFixture {
        {}, vthost::NativeSessionServices { .handshake = { .acceptedTokens = { { .token = "right" } } } }
    };
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    CHECK(controller.reconnectSupport() == contour::session::ReconnectSupport::Supported);

    auto const refused = controller.connectAndWait(10s);
    REQUIRE_FALSE(refused.has_value());
    CHECK(controller.connectionState() == ConnectionState::Failed); // a first connect is not retried
    CHECK_FALSE(controller.failure().empty());

    // The reactor has recorded the failure, but the GUI has not yet handled connectionClosed: its stop() has
    // not run, and a reconnect now would start a run that the stale handler then stops.
    daemon.serveAgain([&] { daemon.services = {}; });
    auto const early = controller.reconnect();
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error() == ReconnectError::ConnectionEnding);
    CHECK(controller.connectionState() == ConnectionState::Failed);

    controller.stop(); // what the GUI's connectionClosed handler does
    auto const started = controller.reconnect();
    REQUIRE(started.has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    CHECK(controller.failure().empty());

    // While the run is up there is nothing to start.
    auto const again = controller.reconnect();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ReconnectError::AlreadyConnected);

    controller.stop();
}

TEST_CASE("NativeController.reconnect.afterTheReactorDied", "[attach][controller][reconnect][manual]")
{
    using vthost::client::ConnectionState;
    // A reactor that dies tells its owner the connection closed, so the owner stop()s the controller and
    // Reconnect is not refused for good.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto closed = std::atomic<int> { 0 };
    auto const connection = QObject::connect(
        &controller,
        &contour::remote::NativeController::connectionClosed,
        &controller,
        [&] { ++closed; },
        Qt::DirectConnection);

    contour::remote::NativeControllerTestAccess::killReactor(controller);
    REQUIRE(waitUntil([&] { return closed.load() == 1; }));
    CHECK(controller.connectionState() == ConnectionState::Failed);
    CHECK_FALSE(controller.failure().empty());

    controller.stop(); // what the GUI's connectionClosed handler does
    QObject::disconnect(connection);
    REQUIRE(controller.reconnect().has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    controller.stop();
}

TEST_CASE("NativeController.reconnect.restartedReactorDiesBeforeConnecting",
          "[attach][controller][reconnect][manual]")
{
    using vthost::client::ConnectionState;
    // A restarted run is Connecting from the moment Reconnect returns: a reactor that dies before it enters
    // Connecting itself must still tell its owner the connection closed, not hide behind the old Failed.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    auto closed = std::atomic<int> { 0 };
    auto const connection = QObject::connect(
        &controller,
        &contour::remote::NativeController::connectionClosed,
        &controller,
        [&] { ++closed; },
        Qt::DirectConnection);

    contour::remote::NativeControllerTestAccess::killReactor(controller);
    REQUIRE(waitUntil([&] { return closed.load() == 1; }));
    CHECK(controller.connectionState() == ConnectionState::Failed);
    controller.stop(); // what the GUI's connectionClosed handler does

    REQUIRE(controller.reconnect().has_value());
    CHECK(controller.connectionState() != ConnectionState::Failed);
    contour::remote::NativeControllerTestAccess::killReactor(controller);
    CHECK(waitUntil([&] { return closed.load() == 2; }));

    controller.stop();
    QObject::disconnect(connection);
    REQUIRE(controller.reconnect().has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    controller.stop();
}

TEST_CASE("NativeController.reconnect.afterAStop", "[attach][controller][reconnect][manual]")
{
    using vthost::client::ConnectionState;
    // The app stops the controller when its connection ends for good (closing every mirror pty); a
    // reconnect after that starts the run again on a fresh reactor.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    controller.stop();
    CHECK(controller.connectionState() == ConnectionState::Stopped);

    REQUIRE(controller.reconnect().has_value());
    CHECK(waitUntil([&] { return controller.connectionState() == ConnectionState::Connected; }));
    controller.stop();
}

TEST_CASE("NativeController.reconnect.neverAttached", "[attach][controller][reconnect][manual]")
{
    auto daemon = DaemonFixture {};
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto const result = controllerOwned->reconnect();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == contour::session::ReconnectError::NotAttached);
}

TEST_CASE("NativeController.reconnect.takesAPaneBackByItsRemoteSession",
          "[attach][controller][reconnect][manual]")
{
    using contour::session::ReconnectBinding;
    // The layout the daemon pushes names the session a lost pane mirrored: the next pty binds to it again,
    // keeping the id. A pane whose session the daemon no longer has is Gone.
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    auto const session = controller.sessionForPty(&pane.pty());
    REQUIRE(session.has_value());

    CHECK(controller.bindForReconnect(&pane.pty()) == ReconnectBinding::Bound);
    // Bound for the next pty, which therefore may be made though nothing is pending.
    CHECK(controller.canCreateSession());
    auto replacement = MirrorPane { route.router, route.router.createPty(std::nullopt) };
    CHECK(controller.sessionForPty(&replacement.pty()) == session);
    CHECK_FALSE(controller.canCreateSession());

    // A pty the controller never handed out names no session.
    auto stranger = vtpty::MockPty { cells(80, 24) };
    CHECK(controller.bindForReconnect(&stranger) == ReconnectBinding::Gone);

    controller.stop();
    replacement.terminal.reset();
    pane.terminal.reset();
}

TEST_CASE("NativeController.reconnect.cancelledBindingLeavesNothingForTheNextPty",
          "[attach][controller][reconnect][manual]")
{
    using contour::session::ReconnectBinding;
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("kept");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    requireConnected(controller);
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    auto route = AttachRoute { controller };
    auto pane = MirrorPane { route.router, route.router.createPty(std::nullopt) };

    REQUIRE(controller.bindForReconnect(&pane.pty()) == ReconnectBinding::Bound);
    REQUIRE(controller.canCreateSession());
    controller.cancelReconnectBinding();
    CHECK_FALSE(controller.canCreateSession());

    controller.stop();
    pane.terminal.reset();
}

TEST_CASE("attach controller reports an unreachable daemon", "[attach][controller]")
{
    // Port 1 on loopback has nothing listening: connect is refused before any TLS.
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} }, timeline);
    auto& controller = *controllerOwned;
    auto const connected = controller.connectAndWait(2s);
    REQUIRE(!connected.has_value());
    CHECK(!connected.error().empty());
}

namespace
{
/// A transport whose every attempt fails with one message.
class FailingTransportFactory final: public vthost::ITransportFactory
{
  public:
    FailingTransportFactory(std::string where, std::string message):
        _where { std::move(where) }, _message { std::move(message) }
    {
    }

    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& /*loop*/) override
    {
        return fail(_message);
    }

    [[nodiscard]] std::string describe() const override { return _where; }

    [[nodiscard]] vthost::PeerTrust peerTrust() const noexcept override
    {
        return vthost::PeerTrust::Verified;
    }

  private:
    [[nodiscard]] static core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    fail(std::string message)
    {
        co_return std::unexpected { vthost::TransportError { .kind = vthost::TransportErrorKind::Refused,
                                                             .message = std::move(message) } };
    }

    std::string _where;
    std::string _message;
};
} // namespace

TEST_CASE("NativeController names the transport of a failure once", "[attach][controller][hosts]")
{
    struct Row
    {
        std::string_view name;
        std::string message;
        std::string expected;
    };
    auto const rows = std::array {
        Row { .name = "a message that names no host gets it appended",
              .message = "connection refused",
              .expected = "connection refused (ssh://me@homepc)" },
        Row { .name = "an ssh message names its host already",
              .message = "ssh could not reach ssh://me@homepc: Connection refused",
              .expected = "ssh could not reach ssh://me@homepc: Connection refused" },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto timeline = TestTimeline {};
        auto controller = contour::remote::NativeController {
            std::make_shared<FailingTransportFactory>("ssh://me@homepc", row.message),
            vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows,
                                          .windows = {} },
            std::string {},
            std::nullopt,
            {},
            std::nullopt,
            core::platform::defaultSteadyClock(),
            timeline.timeline
        };
        auto const connected = controller.connectAndWait(10s);
        REQUIRE_FALSE(connected.has_value());
        CHECK(connected.error() == row.expected);
        controller.stop();
    }
}

namespace
{
/// An ssh child that never answers: its stdout stays open and silent, like a remote stuck in its login.
class SilentStdioChild final: public vthost::IStdioChild
{
  public:
    explicit SilentStdioChild(std::unique_ptr<core::net::ISocket> remoteEnd):
        _remoteEnd { std::move(remoteEnd) }
    {
    }

    [[nodiscard]] std::string stderrTail() const override { return {}; }
    [[nodiscard]] std::optional<int> exitCode(std::chrono::milliseconds /*wait*/) override
    {
        return std::nullopt;
    }
    void terminate() noexcept override { _remoteEnd.reset(); }

  private:
    std::unique_ptr<core::net::ISocket> _remoteEnd; ///< Held open, written never.
};

/// Spawns SilentStdioChild, on an in-process socket pair.
class SilentStdioSpawner final: public vthost::IStdioSpawner
{
  public:
    [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
        core::net::EventLoop& loop, std::span<std::string const> /*argv*/) override
    {
        auto pair = core::net::testing::makeSocketPair(loop);
        if (!pair)
            return std::unexpected { vthost::StdioSpawnError { .kind = vthost::SpawnFailure::SystemError,
                                                               .message = "no socket pair" } };
        return vthost::SpawnedStdioChild { .transport = std::move(pair->first),
                                           .process =
                                               std::make_unique<SilentStdioChild>(std::move(pair->second)) };
    }
};
} // namespace

TEST_CASE("a silent ssh remote is reported by the ssh transport rather than as an attach timeout",
          "[attach][controller][hosts]")
{
    // The attach's clock starts before the transport's: with the same bound for both, the attach would
    // time out first and say nothing about the host. Above MinimumConnectAwait, so the floor decides nothing.
    auto constexpr TransportBound = std::chrono::milliseconds { 2500 };
    static_assert(TransportBound > contour::remote::MinimumConnectAwait);
    auto timeline = TestTimeline {};
    auto controller = contour::remote::NativeController {
        std::make_shared<vthost::SshTransportFactory>(
            vthost::SshTarget { .user = "me", .host = "homepc", .port = std::nullopt },
            std::make_shared<SilentStdioSpawner>(),
            TransportBound),
        vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows, .windows = {} },
        std::string {},
        std::nullopt,
        {},
        std::nullopt,
        core::platform::defaultSteadyClock(),
        timeline.timeline
    };
    auto const connected = controller.connectAndWait(contour::cli::sshAttachBudget(TransportBound));
    REQUIRE_FALSE(connected.has_value());
    CHECK(connected.error() == "ssh://me@homepc did not answer within 3 seconds");
    controller.stop();
}

// B4: the manager's attach-window seam (consumeAttachWindow) routes a freshly-spawned
// window to the binder ContourGuiApp installs — the QML-side hook that lets a spawned
// window adopt a daemon window instead of creating a fresh first tab. Without a binder
// it is inert (like an ordinary local window).
TEST_CASE("consumeAttachWindow routes a spawned window to the installed binder", "[attach][factory]")
{
    auto app = contour::test::TestApp { std::make_unique<RefusingFactory>() };
    auto controller = contour::test::ScopedController { app.manager() };

    // No binder installed (an ordinary local run): the seam is a no-op.
    CHECK_FALSE(app.manager().consumeAttachWindow(controller.controller));
    CHECK_FALSE(app.manager().consumeAttachWindow(nullptr));

    // An installed binder is consulted with the exact window being bootstrapped, and its
    // verdict is returned verbatim.
    contour::window::WindowController* seen = nullptr;
    auto verdict = false;
    app.manager().setAttachWindowBinder([&](contour::window::WindowController* c) {
        seen = c;
        return verdict;
    });
    CHECK_FALSE(app.manager().consumeAttachWindow(controller.controller));
    CHECK(seen == controller.controller);
    verdict = true;
    CHECK(app.manager().consumeAttachWindow(controller.controller));
    // A null controller never reaches the binder.
    seen = nullptr;
    CHECK_FALSE(app.manager().consumeAttachWindow(nullptr));
    CHECK(seen == nullptr);
}

TEST_CASE("a refusing session factory blocks every creation entry point", "[attach][factory]")
{
    auto app = contour::test::TestApp { std::make_unique<RefusingFactory>() };
    auto controller = contour::test::ScopedController { app.manager() };

    CHECK(app.manager().createSessionInBackground(controller.id) == nullptr);
    CHECK(app.manager().model().window(controller.id)->tabCount() == 0);
}

// Every SessionFactory verb has to survive the trip through RoutingSessionFactory, which the app
// installs permanently and re-routes when attach mode starts. A verb it forgets to forward is not a
// compile error and not a test failure anywhere else -- the delegate simply never hears about it.
//
// `bindTerminal` was exactly that: it reached the router and stopped, so a daemon-hosted pane in the
// real GUI never got its ScreenMirror and rendered nothing at all, while every headless test passed
// because they call NativeController::bindTerminal directly.
TEST_CASE("the routing session factory forwards every verb to its delegate", "[attach][factory]")
{
    struct RecordingFactory final: contour::session::SessionFactory
    {
        int ptys = 0;
        int tabs = 0;
        int splits = 0;
        int windows = 0;
        int closes = 0;
        int binds = 0;
        int ratios = 0;
        int moves = 0;
        std::vector<uint64_t> movedSessions {}; ///< What the last move request named.
        double lastRatio = 0.0;
        vtpty::Pty const* boundPty = nullptr;
        vtpty::Pty const* tabPty = nullptr; ///< The window the last tab request named.
        vtbackend::Terminal* boundTerminal = nullptr;

        [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string>,
                                                            std::optional<vtbackend::PageSize>,
                                                            std::optional<vtpty::Process::ExecInfo>,
                                                            std::optional<std::string>) override
        {
            ++ptys;
            return nullptr;
        }
        [[nodiscard]] bool canCreateSession() const noexcept override { return false; }
        [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
        {
            return contour::session::SessionHosting::Remote;
        }
        [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty,
                                            contour::session::RemoteSpawn const&) override
        {
            ++tabs;
            tabPty = actingPty;
            return true;
        }
        [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const*,
                                              bool,
                                              contour::session::RemoteSpawn const&) override
        {
            ++splits;
            return true;
        }
        void reportSplitRatio(vtpty::Pty const*, vtpty::Pty const*, double ratio) override
        {
            ++ratios;
            lastRatio = ratio;
        }
        [[nodiscard]] contour::session::MoveSupport moveSupport() const noexcept override
        {
            return contour::session::MoveSupport::Supported;
        }
        [[nodiscard]] std::optional<uint64_t> daemonInstance() const noexcept override { return 7; }
        [[nodiscard]] std::optional<uint64_t> hostSessionIdOf(vtpty::Pty const&) const noexcept override
        {
            return 42;
        }
        [[nodiscard]] contour::session::RemoteRouting requestRemoteMove(
            contour::session::RemoteMove const& move) override
        {
            ++moves;
            movedSessions = move.sourceSessions;
            return contour::session::RemoteRouting::Routed;
        }
        [[nodiscard]] bool requestRemoteWindow(contour::session::RemoteSpawn const&) override
        {
            ++windows;
            return true;
        }
        void requestRemoteClose(vtpty::Pty const*) override { ++closes; }
        void bindTerminal(vtpty::Pty const* pty,
                          vtbackend::Terminal& terminal,
                          contour::session::InterruptionSink const& /*interruptions*/) override
        {
            ++binds;
            boundPty = pty;
            boundTerminal = &terminal;
        }
    };

    auto delegate = RecordingFactory {};
    auto router = contour::remote::RoutingSessionFactory { std::make_unique<RecordingFactory>() };
    router.setDelegate(&delegate);

    auto events = vtbackend::Terminal::NullEvents {};
    auto settings = vtbackend::Settings {};
    auto pty = std::make_unique<vtpty::MockPty>(settings.pageSize);
    auto* device = pty.get();
    auto terminal = vtbackend::Terminal { events,
                                          core::defaultEnvironment(),
                                          core::platform::NativeFileSystem::instance(),
                                          std::move(pty),
                                          std::move(settings),
                                          std::nullopt,
                                          vtbackend::TerminalClocks::system(),
                                          vtbackend::randomSemanticBlockTokens() };

    CHECK(router.createPty(std::nullopt) == nullptr);
    CHECK_FALSE(router.canCreateSession());
    CHECK(router.hosting() == contour::session::SessionHosting::Remote);
    CHECK(router.requestRemoteTab(device, {}));
    CHECK(router.requestRemoteSplit(device, true, {}));
    CHECK(router.requestRemoteWindow({}));
    router.requestRemoteClose(device);
    router.reportSplitRatio(device, device, 0.7);
    router.bindTerminal(device, terminal, {});
    CHECK(router.moveSupport() == contour::session::MoveSupport::Supported);
    CHECK(router.daemonInstance() == 7);
    CHECK(router.hostSessionIdOf(*device) == 42);
    CHECK(router.requestRemoteMove({ .source = vtworkspace::MoveSourceKind::Pane,
                                     .sourceSessions = { 5 },
                                     .target = vtworkspace::MoveTargetKind::Edge,
                                     .targetSession = 6,
                                     .side = vtworkspace::SplitSide::Right,
                                     .index = 0,
                                     .request = 15 })
          == contour::session::RemoteRouting::Routed);

    CHECK(delegate.ptys == 1);
    CHECK(delegate.tabs == 1);
    CHECK(delegate.tabPty == device); // the acting pane names the target WINDOW; it must be routed
    CHECK(delegate.splits == 1);
    CHECK(delegate.windows == 1);
    CHECK(delegate.closes == 1);
    CHECK(delegate.ratios == 1);
    CHECK(delegate.lastRatio == 0.7);
    CHECK(delegate.binds == 1);
    CHECK(delegate.moves == 1);
    CHECK(delegate.movedSessions == std::vector<uint64_t> { 5 });
    CHECK(delegate.boundPty == device);
    CHECK(delegate.boundTerminal == &terminal);
}

namespace
{

/// @return A restore of two windows holding three and four single-pane tabs.
[[nodiscard]] vthost::WorkspaceRestore twoWindowsSevenTabs()
{
    auto snapshot = vthost::WorkspaceSnapshot {};
    for (auto const tabs: { 3, 4 })
    {
        auto window = vthost::WindowSnapshot {};
        window.tabs.resize(static_cast<size_t>(tabs));
        snapshot.windows.push_back(std::move(window));
    }
    return vthost::WorkspaceRestore { .snapshot = std::move(snapshot) };
}

} // namespace

TEST_CASE("a controller attached after a daemon restore is told what came back, once",
          "[attach][controller][restore]")
{
    auto daemon = DaemonFixture { {}, {}, 1, {}, twoWindowsSevenTabs() };
    auto timeline = TestTimeline {};

    auto const firstOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& first = *firstOwned;
    // The signals fire on the reactor thread; queued delivery keeps what they record on this thread, in
    // the order they were raised.
    auto const context = QObject {};
    auto restoredTold = std::vector<std::pair<quint32, quint32>> {};
    auto stateSignals = size_t { 0 };
    auto stateSeenByLastSignal = vthost::client::ConnectionState::Connecting;
    QObject::connect(
        &first,
        &contour::remote::NativeController::workspaceRestored,
        &context,
        [&](quint32 windows, quint32 tabs) { restoredTold.emplace_back(windows, tabs); },
        Qt::QueuedConnection);
    QObject::connect(
        &first,
        &contour::remote::NativeController::connectionStateChanged,
        &context,
        [&] {
            ++stateSignals;
            stateSeenByLastSignal = first.connectionState();
        },
        Qt::QueuedConnection);
    REQUIRE(first.connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return restoredTold.size() == 1; }));
    CHECK(restoredTold.front() == std::pair<quint32, quint32> { 2, 7 });

    // A second client of the same daemon run is told nothing.
    auto const secondOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& second = *secondOwned;
    auto untold = std::vector<std::pair<quint32, quint32>> {};
    QObject::connect(
        &second,
        &contour::remote::NativeController::workspaceRestored,
        &context,
        [&](quint32 windows, quint32 tabs) { untold.emplace_back(windows, tabs); },
        Qt::QueuedConnection);
    REQUIRE(second.connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return second.layout().has_value(); }));
    CHECK(untold.empty());

    // Nor does the first one's reconnect to it raise the summary again. The reactor raises the summary (if
    // it were to) just before the state signal that announces Connected, and queued signals arrive in
    // order, so once this thread has seen that state signal the summary has had its chance. A reconnect
    // raises at least two state signals: Reconnecting, then Connected.
    REQUIRE(waitUntil([&] {
        return first.connectionState() == vthost::client::ConnectionState::Connected
               && stateSeenByLastSignal == vthost::client::ConnectionState::Connected;
    }));
    auto const signalsBeforeDrop = stateSignals;
    daemon.dropConnections();
    REQUIRE(waitUntil([&] {
        return stateSignals >= signalsBeforeDrop + 2
               && first.connectionState() == vthost::client::ConnectionState::Connected
               && stateSeenByLastSignal == vthost::client::ConnectionState::Connected;
    }));
    CHECK(restoredTold.size() == 1);

    first.stop();
    second.stop();
}

TEST_CASE("a hello that restored nothing raises no summary", "[attach][controller][restore]")
{
    auto daemon = DaemonFixture {};
    std::ignore = daemon.seedSession("plain");
    auto timeline = TestTimeline {};
    auto const controllerOwned = makeNativeController(daemon.endpoint(), timeline);
    auto& controller = *controllerOwned;
    auto const context = QObject {};
    auto told = std::vector<std::pair<quint32, quint32>> {};
    QObject::connect(
        &controller,
        &contour::remote::NativeController::workspaceRestored,
        &context,
        [&](quint32 windows, quint32 tabs) { told.emplace_back(windows, tabs); },
        Qt::QueuedConnection);
    REQUIRE(controller.connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return controller.layout().has_value(); }));
    CHECK(told.empty());
    controller.stop();
}

TEST_CASE("a client attaching to a titled multi-pane layout shows each tab's own title (#2187)",
          "[attach][controller][sync-back]")
{
    auto const titled = [](std::string title) {
        auto tab = vtworkspace::LayoutTab {};
        tab.title = std::move(title);
        tab.root.orientation = vtworkspace::SplitState::Vertical;
        auto nested = vtworkspace::LayoutPane {};
        nested.orientation = vtworkspace::SplitState::Horizontal;
        nested.children = { vtworkspace::LayoutPane {}, vtworkspace::LayoutPane {} };
        tab.root.children = { vtworkspace::LayoutPane {}, std::move(nested), vtworkspace::LayoutPane {} };
        return tab;
    };
    auto capture = SharedLogCapture {};
    auto daemon = DaemonFixture {
        {}, {}, 1, {}, std::nullopt, vtworkspace::Layout { .tabs = { titled("editor"), titled("logs") } }
    };

    auto gui = AttachedGui { daemon };
    requireConnected(*gui.controller);
    REQUIRE(waitUntil([&] {
        auto const layout = gui.controller->layout();
        return layout && layout->tabs.size() == 2;
    }));
    gui.reconcile();

    auto* const win = gui.app.manager().model().window(gui.window.id);
    REQUIRE(win != nullptr);
    REQUIRE(win->tabCount() == 2);
    CHECK(win->tabAt(0)->runtimeTitle() == "editor");
    CHECK(win->tabAt(1)->runtimeTitle() == "logs");

    // Nothing the client applied went back to the daemon as an edit, and the daemon still has both titles.
    gui.reconcile();
    std::this_thread::sleep_for(100ms);
    CHECK(editTabsReceived(capture.text()) == 0);
    auto const titles = daemon.onDaemon([&daemon] {
        auto const* window = daemon.host->model().window(daemon.host->windowId());
        return window->tabAt(0)->runtimeTitle().value_or("") + ","
               + window->tabAt(1)->runtimeTitle().value_or("");
    });
    CHECK(titles == "editor,logs");

    gui.controller->stop();
}
