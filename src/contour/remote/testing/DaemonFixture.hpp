// SPDX-License-Identifier: Apache-2.0
#pragma once

// The in-process daemon the remote/ tests attach to, and the controller they attach with: a `contour daemon`
// on its own event loop, serving TLS on loopback, with MockPty sessions. Shared by NativeController_test and
// DaemonLink_test; test support only, linked into no shipped binary.

#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/NativeController.hpp>

#include <vtbackend/screen/Settings.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>

#include <core/Random.hpp>
#include <core/Utils.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/SystemPower.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ClientSizePolicy.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/ProfileCatalog.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/client/KeySource.hpp>
#include <vthost/latency/StartupTimeline.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/testing/CountingRandom.hpp>
#include <vtworkspace/LayoutTree.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Primitives.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

namespace contour::remote::testing
{

using namespace std::chrono_literals;

/// The timeline a test controller stamps, on a clock nothing advances.
struct TestTimeline
{
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} };
    vthost::latency::StartupTimeline timeline { clock, clock.now() };
};

/// What makeNativeController() builds a controller with beyond its daemon and timeline.
struct ControllerOptions
{
    /// Asked before each session the controller creates; empty to keep none.
    contour::remote::NativeController::SessionSettingsSource sessionSettings = {};
    /// Asked as each hello is built for the light/dark preference it states; empty states none.
    contour::remote::NativeController::ColorPreferenceSource colorPreference = {};
    /// The emulation settings its hello states, the profile page size among them; none states none, and so
    /// no client area.
    std::optional<vtbackend::Settings> attachSettings = std::nullopt;
    /// Where it hears of system sleep and wake; null for nowhere. It must outlive the controller.
    core::platform::ISystemPowerMonitor* powerMonitor = nullptr;
    /// How long its reconnects wait after a loss; the production schedule by default.
    vthost::client::BackoffSchedule backoff = {};
    /// The directory its first hello asks the attach's session to start in; empty for the profile's.
    std::filesystem::path workingDirectory = {};
    /// Which of the daemon's workspaces it shows; a launch of everything left by default. A test of two
    /// clients that share a session names the workspace in each: a second plain launch is given a workspace
    /// of its own.
    vthost::proto::AttachIntent intent = { .kind = vthost::proto::AttachIntentKind::Default, .workspace = 0 };
    /// Whether the daemon may ask it to switch its windows; it may, as the GUI's.
    contour::remote::NativeController::SwitchRequests switchRequests =
        contour::remote::NativeController::SwitchRequests::Answered;
};

/// A TLS transport to @p endpoint, trusting what the endpoint's expected fingerprint says and keeping its
/// pins in memory, as a test has no file to keep them in.
/// @param endpoint The daemon, as DaemonFixture::endpoint() states it.
/// @param keySources What signs in by key; none for a client with only a token.
/// @return The transport; it owns the pins it checks.
[[nodiscard]] inline std::shared_ptr<vthost::TlsTransportFactory> makeTestTlsTransport(
    vthost::TcpEndpoint endpoint, vthost::client::ClientKeySourceProvider keySources = {})
{
    struct Pins
    {
        core::platform::testing::InMemoryFileSystem files;
        vthost::testing::CountingRandom random {};
        vthost::KnownDaemons known { files, "/pins/known_daemons", random };
    };
    auto pins = std::make_shared<Pins>();
    auto* const factory =
        new vthost::TlsTransportFactory { std::move(endpoint), pins->known, nullptr, std::move(keySources) };
    return std::shared_ptr<vthost::TlsTransportFactory> { factory, [pins](vthost::TlsTransportFactory* p) {
                                                             delete p;
                                                         } };
}

/// What a controller is built with in every test here: no attach settings of its own, on the steady clock.
/// The one place its configuration is spelled, so a collaborator it gains is added here alone.
/// @param endpoint The daemon to attach to, over TLS.
/// @param timeline What the controller stamps; it must outlive the controller.
/// @param options What else it is built with; none of it by default.
/// @return The configuration.
[[nodiscard]] inline contour::remote::NativeController::Config controllerConfig(
    vthost::TcpEndpoint endpoint, TestTimeline& timeline, ControllerOptions options = {})
{
    return contour::remote::NativeController::Config {
        .transport = makeTestTlsTransport(std::move(endpoint)),
        .intent = options.intent,
        .profileName = std::string {},
        .workingDirectory = std::move(options.workingDirectory),
        .attachSettings = std::move(options.attachSettings),
        .sessionSettings = std::move(options.sessionSettings),
        .colorPreference = std::move(options.colorPreference),
        .clock = core::platform::defaultSteadyClock(),
        .timeline = timeline.timeline,
        .powerMonitor = options.powerMonitor,
        .backoff = options.backoff,
        .switchRequests = options.switchRequests,
    };
}

/// Builds a controller as every test here does (@see controllerConfig()).
/// @param endpoint The daemon to attach to, over TLS.
/// @param timeline What the controller stamps; it must outlive the controller.
/// @param options What else it is built with; none of it by default.
/// @return The controller, not yet connecting.
[[nodiscard]] inline std::unique_ptr<contour::remote::NativeController> makeNativeController(
    vthost::TcpEndpoint endpoint, TestTimeline& timeline, ControllerOptions options = {})
{
    return std::make_unique<contour::remote::NativeController>(
        controllerConfig(std::move(endpoint), timeline, std::move(options)));
}

/// Stops a controller when it goes out of scope, so the reactor thread is gone before any local declared
/// before this guard -- a signal handler's captures, a log -- is destroyed, whether the case ends or a
/// failing REQUIRE unwinds it. Declare it after everything a handler connected to the controller captures.
class StopOnExit
{
  public:
    explicit StopOnExit(contour::remote::NativeController& controller): _controller(controller) {}
    ~StopOnExit() { _controller.stop(); }

    StopOnExit(StopOnExit const&) = delete;
    StopOnExit& operator=(StopOnExit const&) = delete;
    StopOnExit(StopOnExit&&) = delete;
    StopOnExit& operator=(StopOnExit&&) = delete;

  private:
    contour::remote::NativeController& _controller;
};

/// The services of a daemon that predates the window's selection: it does not offer
/// Capability::DaemonFollow, so a controller cannot attach to it.
/// @return The services to give a DaemonFixture.
[[nodiscard]] inline vthost::NativeSessionServices servicesWithoutDaemonFollow()
{
    auto services = vthost::NativeSessionServices {};
    std::erase(services.handshake.capabilities.idents,
               std::to_underlying(vthost::proto::Capability::DaemonFollow));
    return services;
}

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

/// How many PDUs of each counted kind the daemon received from its clients, across every connection. The
/// kinds are the rows of @c Kinds: counting another is one more row. Written on the daemon thread, read by
/// any. Every read fails the case if the client's bytes did not decode, for what it reads is then incomplete.
class ClientFrameCounts
{
  public:
    /// The PDU kinds counted.
    static constexpr auto Kinds = std::array { vthost::proto::PduType::ResizeRequest,
                                               vthost::proto::PduType::Heartbeat,
                                               vthost::proto::PduType::ResizePane };

    /// @param type A counted kind.
    /// @return How many PDUs of @p type arrived so far; 0 for a kind that is not counted.
    [[nodiscard]] uint64_t count(vthost::proto::PduType type) const
    {
        checkDecoded();
        auto const row = std::ranges::find(Kinds, type);
        return row == Kinds.end() ? 0 : _counts[static_cast<std::size_t>(row - Kinds.begin())].load();
    }

    /// Counts @p pdu if it is of a counted kind, and keeps the area a ResizeRequest states.
    void note(vthost::proto::DecodedPdu const& pdu)
    {
        if (auto const row = std::ranges::find(Kinds, vthost::proto::typeOf(pdu)); row != Kinds.end())
            ++_counts[static_cast<std::size_t>(row - Kinds.begin())];
        if (auto const* resize = std::get_if<vthost::proto::ResizeRequest>(&pdu))
        {
            auto const lock = std::lock_guard { _areasMutex };
            _areas.push_back(
                vtpty::PageSize { .lines = vtpty::LineCount(static_cast<int>(resize->lines)),
                                  .columns = vtpty::ColumnCount(static_cast<int>(resize->columns)) });
            _requestCells.push_back(resize->cellPixelSize);
        }
        if (auto const* pane = std::get_if<vthost::proto::ResizePane>(&pdu))
        {
            auto const lock = std::lock_guard { _areasMutex };
            _paneCells.push_back(pane->cellPixelSize);
        }
        if (auto const* hello = std::get_if<vthost::proto::ClientHello>(&pdu))
        {
            auto const lock = std::lock_guard { _areasMutex };
            _helloCells.push_back(hello->cellPixelSize);
            _helloDirectories.push_back(hello->workingDirectory);
            _helloHistories.push_back(hello->sessionSettings.transform(
                [](auto const& settings) { return settings.historyLineCount; }));
        }
    }

    /// @return The cell size each ResizeRequest stated so far (nullopt for none), in arrival order.
    [[nodiscard]] std::vector<std::optional<vthost::proto::WireCellPixelSize>> requestCells() const
    {
        checkDecoded();
        auto const lock = std::lock_guard { _areasMutex };
        return _requestCells;
    }

    /// @return The cell size each ResizePane stated so far (nullopt for none), in arrival order.
    [[nodiscard]] std::vector<std::optional<vthost::proto::WireCellPixelSize>> paneCells() const
    {
        checkDecoded();
        auto const lock = std::lock_guard { _areasMutex };
        return _paneCells;
    }

    /// @return The cell size each ClientHello stated so far (nullopt for none), in arrival order.
    [[nodiscard]] std::vector<std::optional<vthost::proto::WireCellPixelSize>> helloCells() const
    {
        checkDecoded();
        auto const lock = std::lock_guard { _areasMutex };
        return _helloCells;
    }

    /// @return The working directory each ClientHello stated so far (empty for none), in arrival order.
    [[nodiscard]] std::vector<std::string> helloDirectories() const
    {
        checkDecoded();
        auto const lock = std::lock_guard { _areasMutex };
        return _helloDirectories;
    }

    /// @return The history limit each ClientHello's session settings stated so far (nullopt for no settings),
    ///         in arrival order.
    [[nodiscard]] std::vector<std::optional<int64_t>> helloHistories() const
    {
        checkDecoded();
        auto const lock = std::lock_guard { _areasMutex };
        return _helloHistories;
    }

    /// @return Every client area a ResizeRequest stated so far, in arrival order.
    [[nodiscard]] std::vector<vtpty::PageSize> areas() const
    {
        checkDecoded();
        auto const lock = std::lock_guard { _areasMutex };
        return _areas;
    }

    /// Records that the client's bytes did not decode, past which the counts above are no longer complete.
    void noteDecodeError() noexcept { ++_decodeErrors; }

  private:
    /// Fails the case if the client's bytes ever failed to decode.
    void checkDecoded() const { CHECK(_decodeErrors.load() == 0); }

    std::array<std::atomic<uint64_t>, Kinds.size()> _counts {};
    std::atomic<uint64_t> _decodeErrors {};
    mutable std::mutex _areasMutex {};
    std::vector<vtpty::PageSize> _areas {};                                        ///< Under _areasMutex.
    std::vector<std::optional<vthost::proto::WireCellPixelSize>> _requestCells {}; ///< Under _areasMutex.
    std::vector<std::optional<vthost::proto::WireCellPixelSize>> _paneCells {};    ///< Under _areasMutex.
    std::vector<std::optional<vthost::proto::WireCellPixelSize>> _helloCells {};   ///< Under _areasMutex.
    std::vector<std::string> _helloDirectories {};                                 ///< Under _areasMutex.
    std::vector<std::optional<int64_t>> _helloHistories {};                        ///< Under _areasMutex.
};

/// A daemon end of a connection that decodes the frames the client sends, keeping partial frames across
/// reads, and counts them into @p counts. Reads pass through unchanged.
class CountingSocket final: public core::net::testing::SocketDecorator
{
  public:
    using ReadResult = std::expected<std::size_t, core::net::NetError>;

    CountingSocket(std::unique_ptr<core::net::ISocket> inner, ClientFrameCounts& counts):
        SocketDecorator(*inner), _inner(std::move(inner)), _counts(counts)
    {
    }

    CountingSocket(CountingSocket const&) = delete;
    CountingSocket& operator=(CountingSocket const&) = delete;
    CountingSocket(CountingSocket&&) = delete;
    CountingSocket& operator=(CountingSocket&&) = delete;

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        return core::net::IoAwaitable { observe(buffer) };
    }

  private:
    [[nodiscard]] core::async::Task<ReadResult> observe(std::span<std::byte> buffer)
    {
        auto const read = co_await _inner->read(buffer);
        if (read && *read > 0)
        {
            _pending.insert(_pending.end(), buffer.begin(), buffer.begin() + static_cast<long>(*read));
            for (;;)
            {
                auto const decoded = vthost::proto::decodePdu(_pending);
                if (!decoded && decoded.error() == vthost::proto::DecodeError::NeedMoreData)
                    break; // incomplete: the rest arrives with the next read
                if (!decoded)
                {
                    // Not a frame boundary this decoder can find again: what follows is not counted, and
                    // the case reading the counts is told so instead of reading a silent undercount.
                    _counts.noteDecodeError();
                    _pending.clear();
                    break;
                }
                _counts.note(decoded->pdu);
                _pending.erase(_pending.begin(), _pending.begin() + static_cast<long>(decoded->consumed));
            }
        }
        co_return read;
    }

    std::unique_ptr<core::net::ISocket> _inner;
    ClientFrameCounts& _counts;
    std::vector<std::byte> _pending {};
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

    /// Holds everything written from now on: the client hears nothing more of the daemon until release().
    void holdNow()
    {
        _holdAtLayout = true;
        _holding = true;
    }

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
            if (layout != nullptr && layout->workspace == *_holdAfter)
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

/// An in-process `contour daemon` (native protocol) on its own thread, serving
/// TLS over a LOOPBACK TCP socket on an EPHEMERAL port. Using TCP (the same
/// transport `client --connect-tcp` uses) rather than AF_UNIX lets these
/// end-to-end tests run on every platform, Windows included.
struct DaemonFixture
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    ClientFrameCounts clientFrames; ///< What the clients sent, counted by kind; outlives the sockets.
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
    /// The fingerprint of the certificate the daemon presents, as a client is told it out of band.
    std::string fingerprint;
    std::thread thread;
    bool cancelled = false; ///< Whether teardown unwound the accept loop.
    uint64_t instance = 1;  ///< Which run of the daemon @c host is; 0 states none.
    /// The host's profiles, by name; none resolves every spawn to the host's own settings.
    std::shared_ptr<vthost::ProfileCatalog const> profiles;
    /// The workspace the first host restores as it starts; none restores nothing.
    std::optional<vthost::WorkspaceRestore> restore;
    /// The tabs the host opens on construction, as `default_layout` would; none opens none.
    vtworkspace::Layout startupLayout;
    /// How the host resolves one client area from several clients' reports.
    vthost::ClientSizePolicy sizePolicy = vthost::ClientSizePolicy::Latest;

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
                                        .sizePolicy = sizePolicy,
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
    /// @param sizePolicy How the host resolves one client area from several; the latest report by default.
    /// @param authorizedKeys The keys the daemon authenticates by (keys only, no token); none for the token
    ///        guard @p services states. Such a daemon is served without the fixture's socket decorators.
    explicit DaemonFixture(std::shared_ptr<vthost::IExtensionHosting> extensions = {},
                           vthost::NativeSessionServices services = {},
                           uint64_t instance = 1,
                           std::shared_ptr<vthost::ProfileCatalog const> profiles = {},
                           std::optional<vthost::WorkspaceRestore> restore = std::nullopt,
                           vtworkspace::Layout startupLayout = {},
                           vthost::ClientSizePolicy sizePolicy = vthost::ClientSizePolicy::Latest,
                           std::vector<vthost::AuthorizedKey> authorizedKeys = {}):
        extensions(std::move(extensions)),
        services(std::move(services)),
        instance(instance),
        profiles(std::move(profiles)),
        restore(std::move(restore)),
        startupLayout(std::move(startupLayout)),
        sizePolicy(sizePolicy)
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
        {
            auto const digest = core::fromHexString(std::string_view { (*tls)->certificateFingerprint() });
            REQUIRE(digest.has_value());
            REQUIRE(digest->size() == 32);
            auto bytes = std::array<std::byte, 32> {};
            std::ranges::transform(*digest, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
            fingerprint = core::net::sshStyleFingerprint(bytes);
        }
        auto const& context = *tls;
        auto const keyed = !authorizedKeys.empty();
        if (keyed)
            native = vthost::makeTlsNativeHandler(loop,
                                                  *host,
                                                  context,
                                                  this->services,
                                                  vthost::TlsKeyAuth { .keys = std::move(authorizedKeys),
                                                                       .auth = vthost::DaemonAuth::KeysOnly,
                                                                       .random = core::systemRandom() })
                         .value();
        else
            native = vthost::makeNativeHandler(loop, *host, this->services);
        auto handler = [this, context, keyed](vthost::ConnectionId id,
                                              std::unique_ptr<core::net::ISocket> socket) {
            // The TLS-and-keys handler wraps the socket itself.
            if (keyed)
                return native(std::move(id), std::move(socket));
            auto gate = std::make_unique<GatedSocket>(
                std::make_unique<CountingSocket>(
                    std::make_unique<DroppableSocket>(context->wrap(std::move(socket), loop), live),
                    clientFrames),
                gated);
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
        return vthost::TcpEndpoint {
            .host = "127.0.0.1", .port = port, .token = {}, .caPem = {}, .expectedFingerprint = fingerprint
        };
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

    /// Has every connection hold everything the daemon writes from now on, until releaseHeld(): what the
    /// daemon does meanwhile, it does with no client having heard of it.
    void holdEverything()
    {
        onDaemon([this] {
            for (auto* socket: gated)
                socket->holdNow();
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
            auto const id = host->model().workspace(host->workspaceId())->activeTab()->rootPane()->session();
            host->terminal(id)->writeToScreen(text);
            return id;
        });
    }

    /// @return The client area the host holds.
    [[nodiscard]] vtpty::PageSize clientArea()
    {
        return onDaemon([this] { return host->pageSize(); });
    }

    /// @return The grid the host's terminal of @p session has; an empty one for a session it does not have.
    [[nodiscard]] vtpty::PageSize paneSize(uint64_t session)
    {
        return onDaemon([this, session] {
            auto* terminal = host->terminal(vtworkspace::SessionId { session });
            return terminal != nullptr ? terminal->pageSize() : vtpty::PageSize {};
        });
    }

    /// Splits the active pane of the host's active tab, as a split the daemon authors.
    /// @param state The split's axis.
    /// @param ratio The share of the first half.
    void splitActive(vtworkspace::SplitState state, double ratio)
    {
        onDaemon([this, state, ratio] {
            auto* tab = host->model().workspace(host->workspaceId())->activeTab();
            host->splitActivePane(tab->id(), state, ratio);
            return 0;
        });
    }
};

/// A workspace added to a daemon, holding one tab of one pane.
struct AddedWorkspace
{
    uint64_t id = 0;                ///< The daemon's workspace id.
    vtworkspace::SessionId session; ///< The session of its one pane.
};

/// Adds a workspace of one tab to @p daemon, as the control API's workspace verbs do.
/// @return The workspace and its pane.
[[nodiscard]] inline AddedWorkspace addWorkspace(DaemonFixture& daemon)
{
    return daemon.onDaemon([&daemon] {
        auto const* workspace = daemon.host->createWorkspace();
        return AddedWorkspace { .id = workspace->id().value,
                                .session = workspace->activeTab()->rootPane()->session() };
    });
}

/// Seeds @p daemon with @p count workspaces of one tab each.
/// @return Their ids, ascending.
inline std::vector<uint64_t> seedWorkspaces(DaemonFixture& daemon, int count)
{
    std::ignore = daemon.seedSession("first");
    auto ids =
        std::vector<uint64_t> { daemon.onDaemon([&daemon] { return daemon.host->workspaceId().value; }) };
    for ([[maybe_unused]] auto const each: std::views::iota(1, count))
        ids.push_back(addWorkspace(daemon).id);
    return ids;
}

/// Whether a wait runs the GUI thread's queued work meanwhile.
enum class GuiEvents : uint8_t
{
    Pumped = 0, ///< Queued work runs, as it would while the GUI waits in its event loop.
    Held,       ///< Nothing queued runs: what the wait sees is what the reactor alone did.
};

/// Polls @p predicate at 5ms intervals until it holds or @p attempts elapse, running the GUI thread's
/// queued work in between unless @p events holds it. Also used inverted, to assert a state is never
/// reached.
[[nodiscard]] inline bool waitUntil(std::function<bool()> const& predicate,
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
inline void drainGuiEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

/// Looks at @p link's attach as the launch does -- without waiting -- until it settled.
/// @param link The link whose attach was started by its constructor.
/// @param budget How long the attach may take.
/// @return Nothing once attached; why not otherwise.
[[nodiscard]] inline std::expected<void, std::string> pollUntilAttached(contour::remote::DaemonLink& link,
                                                                        vthost::AttachBudget budget)
{
    while (true)
    {
        if (auto const settled = link.pollAttached(budget))
            return *settled;
        std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
    }
}

} // namespace contour::remote::testing
