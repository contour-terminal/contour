// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h (which project headers like vtpty's pull
// in), or the old winsock definitions clash with it — so this block leads
// the translation unit, mirroring every Win32 net TU.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
// clang-format on

#include <muxserver/Daemon.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <coro/WhenAll.hpp>
#include <coro/WhenAny.hpp>
#include <muxserver/MuxServer.h>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/SocketPath.h>
#include <muxserver/client/AttachClient.h>
#include <muxserver/client/ScreenMirror.h>
#include <muxserver/tmux/ControlSession.h>
#include <muxserver/tmux/ImsgServer.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <net/Tls.h>

#ifndef _WIN32
    #include <sys/ioctl.h>

    #include <csignal>

    #include <fcntl.h>
    #include <termios.h>
    #include <unistd.h>

    #include <net/posix/FdUtils.h>
#endif

namespace muxserver
{

namespace
{
    /// Drives every protocol server's accept loop on the one reactor.
    coro::Task<void> serveAll(std::vector<MuxServer*> servers)
    {
        auto accepts = std::vector<coro::Task<void>> {};
        accepts.reserve(servers.size());
        for (auto* server: servers)
            accepts.push_back(server->serve());
        co_await coro::whenAll(std::move(accepts));
    }

    /// Wires the three mirror handlers (screen delta, image, session event) onto @p attach, all
    /// re-serializing through @p mirror and emitting via @p writeOut. Single-sources the identical
    /// wiring the POSIX and Windows attach flows share — only @p writeOut (the platform's stdout /
    /// console emit) differs — so the two cannot silently drift.
    /// @param attach The client whose handlers are set.
    /// @param mirror The re-serialization state feeding the outer terminal.
    /// @param activeSession Tracks the session of each screen delta (the focused pane).
    /// @param writeOut Emits the reserialized bytes to the platform's stdout/console.
    template <typename WriteOut>
    void wireMirrorHandlers(client::AttachClient& attach,
                            client::ScreenMirror& mirror,
                            std::uint64_t& activeSession,
                            WriteOut const& writeOut)
    {
        attach.setUpdateHandler([&activeSession, &mirror, &writeOut](client::RemoteScreen const& screen,
                                                                     proto::Delta const& delta) {
            activeSession = screen.session;
            writeOut(mirror.apply(screen, delta));
        });
        attach.setImageHandler(
            [&mirror, &writeOut](client::RemoteScreen const& screen, std::uint32_t imageId) {
                writeOut(mirror.applyImage(screen, imageId));
            });
        attach.setSessionEventHandler(
            [&writeOut](client::RemoteScreen const& /*screen*/, proto::SessionEvent const& event) {
                writeOut(client::ScreenMirror::applyEvent(event));
            });
    }

    /// The daemon's PTY factory: every session spawns the configured shell over a
    /// fresh PTY. Shared by the POSIX and Windows runDaemon paths.
    [[nodiscard]] PtyFactory makeShellPtyFactory(vtpty::Process::ExecInfo shell)
    {
        return [shell = std::move(shell)](vtbackend::PageSize pageSize) -> std::unique_ptr<vtpty::Pty> {
            return std::make_unique<vtpty::Process>(
                shell, vtpty::createPty(pageSize, std::nullopt), /*escapeSandbox=*/true);
        };
    }

    /// Reads the whole file at @p path, or nullopt if it cannot be opened.
    [[nodiscard]] std::optional<std::string> readFileToString(std::string const& path)
    {
        auto file = std::ifstream { path, std::ios::binary };
        if (!file)
            return std::nullopt;
        return std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    }

    /// Builds the TLS server context for the native TCP listener: an ephemeral
    /// self-signed certificate when none is configured (the TOFU default), else
    /// the configured PEM cert + key. The TCP transport is always encrypted.
    /// @return The context, or nullptr after printing why it could not be built.
    [[nodiscard]] std::shared_ptr<net::ITlsContext> makeNativeTcpTls(NativeTcpListenerConfig const& config)
    {
        if (config.tlsCertPath.empty() && config.tlsKeyPath.empty())
        {
            auto context = net::makeSelfSignedServerContext();
            if (!context)
            {
                std::println(stderr, "contour daemon: TLS: {}", context.error());
                return nullptr;
            }
            std::println(stderr, "contour daemon: native TCP using an ephemeral self-signed certificate");
            return *context;
        }
        auto const cert = readFileToString(config.tlsCertPath);
        auto const key = readFileToString(config.tlsKeyPath);
        if (!cert || !key)
        {
            std::println(stderr, "contour daemon: cannot read TLS certificate or key file");
            return nullptr;
        }
        auto context = net::makeTlsServerContext(*cert, *key);
        if (!context)
        {
            std::println(stderr, "contour daemon: TLS: {}", context.error());
            return nullptr;
        }
        return *context;
    }

    /// Composes a native connection handler that FIRST encrypts each accepted
    /// socket (server-side TLS), then serves the native protocol over it.
    [[nodiscard]] ConnectionHandler makeTlsNativeHandler(net::EventLoop& loop,
                                                         SessionHost& host,
                                                         std::shared_ptr<net::ITlsContext> tls,
                                                         std::string token)
    {
        auto base = makeNativeHandler(loop, host, std::move(token));
        return [tls = std::move(tls), base = std::move(base)](std::unique_ptr<net::ISocket> socket) {
            return base(tls->wrap(std::move(socket)));
        };
    }

    /// Binds a unix listener at @p path, or prints the error and yields the process
    /// exit code the caller should return. Collapses the bind/report/return block
    /// every daemon endpoint otherwise repeats verbatim.
    [[nodiscard]] std::expected<std::unique_ptr<net::IListener>, int> bindDaemonEndpoint(
        net::EventLoop& loop, std::string const& path)
    {
        auto listener = net::listenUnix(loop, path);
        if (!listener)
        {
            std::println(stderr, "contour daemon: {}", listener.error().toString());
            return std::unexpected(EXIT_FAILURE);
        }
        return std::move(*listener);
    }
} // namespace

std::string endpointToken(AttachEndpoint const& endpoint)
{
    if (auto const* tcp = std::get_if<TcpEndpoint>(&endpoint))
        return tcp->token;
    return {};
}

coro::Task<std::expected<std::unique_ptr<net::ISocket>, std::string>> connectAttach(net::EventLoop* loop,
                                                                                    AttachEndpoint endpoint)
{
    if (auto const* unixEp = std::get_if<UnixEndpoint>(&endpoint))
    {
        auto socket = co_await net::connectUnix(loop, nativeSocketPath(unixEp->socketPath).string());
        if (!socket)
            co_return std::unexpected(socket.error().toString());
        co_return std::move(*socket);
    }

    auto const& tcp = std::get<TcpEndpoint>(endpoint);
    auto socket = co_await net::connect(loop, tcp.host, tcp.port);
    if (!socket)
        co_return std::unexpected(socket.error().toString());
    auto tls = net::makeTlsClientContext(tcp.caPem);
    if (!tls)
        co_return std::unexpected(tls.error());
    auto encrypted = (*tls)->wrap(std::move(*socket));
    if (!encrypted)
        co_return std::unexpected(std::string { "TLS handshake setup failed" });
    co_return std::move(encrypted);
}

std::optional<std::pair<std::string, std::uint16_t>> parseHostPort(std::string_view spec)
{
    // Split on the LAST colon so bare IPv6 works when bracketed as [addr]:port.
    auto const colon = spec.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == spec.size())
        return std::nullopt;
    auto host = spec.substr(0, colon);
    auto const portText = spec.substr(colon + 1);
    // Strip one layer of [] around an IPv6 literal.
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (host.empty())
        return std::nullopt;
    auto port = 0U;
    auto const* const first = portText.data();
    auto const* const last = portText.data() + portText.size();
    auto const [ptr, ec] = std::from_chars(first, last, port);
    if (ec != std::errc {} || ptr != last || port == 0 || port > 65535)
        return std::nullopt;
    return std::pair { std::string { host }, static_cast<std::uint16_t>(port) };
}

#ifndef _WIN32

int runDaemon(DaemonConfig const& config)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    auto host = SessionHost { loop, makeShellPtyFactory(config.shell), config.settings };

    auto listener = bindDaemonEndpoint(loop, config.socketPath.string());
    if (!listener)
        return listener.error();
    auto server = MuxServer { loop, std::move(*listener), tmux::makeControlModeHandler(loop, host) };

    // The native cells+deltas protocol listens beside the control-mode socket.
    auto const nativePath = nativeSocketPath(config.socketPath).string();
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer = MuxServer { loop, std::move(*nativeListener), makeNativeHandler(loop, host) };

    // The imsg endpoint serves the REAL tmux client binary
    // (`tmux -S <socket>-tmux -C attach-session`).
    auto const imsgPath = tmuxSocketPath(config.socketPath).string();
    auto imsgListener = bindDaemonEndpoint(loop, imsgPath);
    if (!imsgListener)
        return imsgListener.error();
    auto imsgServer = MuxServer { loop, std::move(*imsgListener), tmux::makeTmuxImsgHandler(loop, host) };

    auto servers = std::vector<MuxServer*> { &server, &nativeServer, &imsgServer };

    // Opt-in: ALSO bind tmux's own discovery path, so a plain
    // `tmux -L <label> -C attach-session` finds this daemon. Opt-in only —
    // with the daemon down, a `new-session` on that path silently forks a
    // REAL tmux server onto it.
    auto compatServer = std::optional<MuxServer> {};
    if (config.tmuxCompatLabel)
    {
        auto const compatPath = std::format("/tmp/tmux-{}/{}", ::getuid(), *config.tmuxCompatLabel);
        auto compatListener = bindDaemonEndpoint(loop, compatPath);
        if (!compatListener)
            return compatListener.error();
        compatServer.emplace(loop, std::move(*compatListener), tmux::makeTmuxImsgHandler(loop, host));
        servers.push_back(&*compatServer);
        std::println(stderr, "contour daemon: tmux-compat socket at {}", compatPath);
    }

    // Opt-in: ALSO serve the native protocol over TCP (loopback by default). The
    // transport is protocol-agnostic, so the same native handler serves it — with
    // the preshared token as the authentication the filesystem gate can't provide.
    auto nativeTcpServer = std::optional<MuxServer> {};
    if (config.nativeTcp)
    {
        auto tcpListener = net::listen(loop, config.nativeTcp->host, config.nativeTcp->port);
        if (!tcpListener)
        {
            std::println(
                stderr, "contour daemon: native TCP listen failed: {}", tcpListener.error().toString());
            return EXIT_FAILURE;
        }
        auto tls = makeNativeTcpTls(*config.nativeTcp);
        if (!tls)
            return EXIT_FAILURE;
        auto const boundPort = (*tcpListener)->localPort();
        nativeTcpServer.emplace(loop,
                                std::move(*tcpListener),
                                makeTlsNativeHandler(loop, host, std::move(tls), config.nativeTcp->token));
        servers.push_back(&*nativeTcpServer);
        std::println(
            stderr, "contour daemon: native TCP listener on {}:{}", config.nativeTcp->host, boundPort);
    }

    // Signal handling without async-signal-safety hazards: SIGINT/SIGTERM are
    // blocked process-wide and consumed by a dedicated sigwait thread, which
    // marshals the shutdown onto the loop via post() (the loop's only
    // thread-safe entry point).
    auto signals = sigset_t {};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    auto signalSeen = std::atomic<bool> { false };
    auto watcher = std::thread { [&] {
        auto sig = 0;
        sigwait(&signals, &sig);
        signalSeen = true;
        loop.post([&] {
            for (auto* each: servers)
                each->close();
            loop.requestStop();
        });
    } };

    std::println(stderr,
                 "contour daemon: serving on {} (native: {}, tmux: {})",
                 config.socketPath.string(),
                 nativePath,
                 imsgPath);
    loop.blockOn(serveAll(servers));

    // Unblock the watcher if shutdown came from elsewhere; if it already
    // consumed a signal, the raised one stays blocked and dies with the process.
    if (!signalSeen)
        std::ignore = ::raise(SIGTERM);
    watcher.join();

    std::println(stderr, "contour daemon: shut down");
    return EXIT_SUCCESS;
}

namespace
{
    /// Puts the controlling TTY into raw mode for the attach lifetime and
    /// restores the saved settings on destruction.
    class RawTty
    {
      public:
        RawTty()
        {
            if (::tcgetattr(STDIN_FILENO, &_saved) != 0)
                return;
            auto raw = _saved;
            ::cfmakeraw(&raw);
            _active = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }

        ~RawTty()
        {
            if (_active)
                ::tcsetattr(STDIN_FILENO, TCSANOW, &_saved);
        }

        RawTty(RawTty const&) = delete;
        RawTty& operator=(RawTty const&) = delete;
        RawTty(RawTty&&) = delete;
        RawTty& operator=(RawTty&&) = delete;

      private:
        termios _saved {};
        bool _active = false;
    };

    /// The SIGWINCH handler's write end of the self-pipe, published by the live
    /// SigwinchNotifier. A signal handler cannot carry user data, so the one active
    /// notifier parks its write fd here; the handler's only job is a one-byte write.
    std::atomic<int> gWinchWriteFd { -1 };

    /// Async-signal-safe SIGWINCH handler: writes one byte to the self-pipe so the
    /// reactor wakes and the resize tracker re-proposes the TTY size. A full pipe
    /// (EAGAIN) already carries a pending wakeup, so the short write is ignored.
    void winchSignalHandler(int /*sig*/)
    {
        auto const fd = gWinchWriteFd.load(std::memory_order_relaxed);
        if (fd >= 0)
        {
            auto const byte = char { 0 };
            std::ignore = ::write(fd, &byte, 1);
        }
    }

    /// Forwards local keystrokes to the attached session; Ctrl-\ detaches.
    coro::Task<void> pumpStdin(net::EventLoop* loop,
                               client::AttachClient* attach,
                               std::uint64_t const* activeSession)
    {
        while (true)
        {
            co_await loop->waitReadable(STDIN_FILENO);
            auto buffer = std::array<char, 512> {};
            auto const n = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (n <= 0)
                break;
            auto const bytes = std::string_view { buffer.data(), static_cast<std::size_t>(n) };
            if (bytes.contains('\x1c')) // Ctrl-\ = detach
                break;
            if (*activeSession != 0)
                attach->sendInput(*activeSession, bytes);
        }
        attach->detach();
    }

    /// Queries the local TTY's size and proposes it to the daemon; a no-op when the
    /// size cannot be read. Called for the initial proposal and again on SIGWINCH.
    void proposeLocalSize(client::AttachClient& attach)
    {
        auto size = winsize {};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0)
            attach.requestResize(size.ws_col, size.ws_row);
    }

    /// The whole attach lifetime: connect, mirror, pump input, detach.
    coro::Task<void> attachFlow(net::EventLoop* loop, AttachEndpoint endpoint, int* exitCode)
    {
        auto const token = endpointToken(endpoint);
        auto socket = co_await connectAttach(loop, std::move(endpoint));
        if (!socket.has_value())
        {
            std::println(stderr, "contour attach: {}", socket.error());
            *exitCode = EXIT_FAILURE;
            co_return;
        }

        auto attach = client::AttachClient { *loop, std::move(*socket), token };
        auto activeSession = std::uint64_t { 0 };
        // The thin client drives the OUTER terminal directly with the same
        // ScreenMirror re-serialization the GUI feeds its mirror Terminal — so
        // OSC 8 hyperlinks, images, mouse-mode propagation, title, colors and
        // cursor shape all reach the real terminal (bounded by its capabilities),
        // not the display-only TtyRenderer repaint.
        auto mirror = client::ScreenMirror {};
        auto const writeOut = [](std::string const& bytes) {
            std::ignore = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        };
        wireMirrorHandlers(attach, mirror, activeSession, writeOut);

        {
            auto const rawMode = RawTty {};
            auto notifier = SigwinchNotifier {};
            // whenAny starts run() first, so its ClientHello precedes the resize
            // proposal in the write queue, and it completes as soon as ANY child
            // finishes — so when the daemon disconnects (run() returns) the parked
            // input pump and resize tracker are cancelled, the RawTty guard restores
            // the terminal, and we exit promptly instead of hanging on stdin.
            std::ignore = co_await coro::whenAny(
                attach.run(),
                pumpStdin(loop, &attach, &activeSession),
                trackTtySize(loop, notifier.readFd(), [&attach] { proposeLocalSize(attach); }));
        }

        // Restore the outer terminal's persistent state (RawTty only restores termios).
        writeOut(mirror.detachRestore());
        std::println("\ncontour attach: detached");
        if (attach.versionMismatch())
        {
            std::println(stderr, "contour attach: daemon speaks an incompatible protocol version");
            *exitCode = EXIT_FAILURE;
        }
    }
} // namespace

SigwinchNotifier::SigwinchNotifier()
{
    auto fds = std::array<int, 2> {};
    if (::pipe(fds.data()) != 0)
        return; // stays invalid: the attach runs without live resize propagation

    _readFd = fds[0];
    _writeFd = fds[1];

    // Close-on-exec so the pipe never leaks into a child; non-blocking so the
    // handler's write and the drain read never stall the loop. Best-effort: a
    // flag that fails to stick degrades behavior, it does not break the pipe.
    std::ignore = net::makeNonBlockingCloexec(_readFd);
    std::ignore = net::makeNonBlockingCloexec(_writeFd);

    // Publish the write end before arming the handler, so a signal that arrives
    // the instant the handler is installed already sees a valid fd.
    gWinchWriteFd.store(_writeFd, std::memory_order_relaxed);

    struct sigaction action = {};
    action.sa_handler = &winchSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART; // auto-restart other syscalls; poll re-polls on EINTR
    if (::sigaction(SIGWINCH, &action, &_previous) != 0)
    {
        // Handler install failed: undo the pipe so readFd() reports InvalidHandle and
        // trackTtySize takes its idle path instead of parking on a dead pipe.
        gWinchWriteFd.store(-1, std::memory_order_relaxed);
        net::platformClose(_writeFd);
        net::platformClose(_readFd);
        _writeFd = net::InvalidHandle;
        _readFd = net::InvalidHandle;
    }
}

SigwinchNotifier::~SigwinchNotifier()
{
    // Restore the prior disposition first so no further signal reaches our handler,
    // then retract the fd and close the pipe: the handler can no longer race the close.
    if (valid())
        std::ignore = ::sigaction(SIGWINCH, &_previous, nullptr);
    gWinchWriteFd.store(-1, std::memory_order_relaxed);
    net::platformClose(_writeFd);
    net::platformClose(_readFd);
}

coro::Task<void> trackTtySize(net::EventLoop* loop, net::NativeHandle winchFd, std::function<void()> propose)
{
    propose(); // initial proposal, ordered after run()'s ClientHello by whenAny start order

    if (winchFd == net::InvalidHandle)
    {
        // No self-pipe: keep the flow alive (and cancellable) without live resize.
        while (true)
            co_await loop->delay(std::chrono::hours(1));
    }

    auto scratch = std::array<char, 64> {};
    while (true)
    {
        co_await loop->waitReadable(winchFd);
        // Coalesce the SIGWINCH burst into a single re-proposal.
        while (true)
        {
            auto const n = ::read(winchFd, scratch.data(), scratch.size());
            if (n <= 0)
                break;
        }
        propose();
    }
}

int runAttach(AttachEndpoint const& endpoint)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    auto exitCode = EXIT_SUCCESS;
    // connectAttach resolves the native socket beside the control socket (unix) or
    // dials TCP+TLS, per the endpoint.
    loop.blockOn(attachFlow(&loop, endpoint, &exitCode));
    return exitCode;
}

#else // _WIN32

namespace
{
    /// The console-control shutdown hook: SetConsoleCtrlHandler's callback is
    /// a plain C function invoked on an arbitrary thread with no user data
    /// pointer, so the running daemon registers its (thread-safe) shutdown
    /// poster in this one process-wide slot.
    std::atomic<std::function<void()>*> gConsoleShutdown = nullptr;

    BOOL WINAPI consoleCtrlHandler(DWORD type)
    {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
            return FALSE;
        if (auto* shutdown = gConsoleShutdown.load())
            (*shutdown)();
        return TRUE;
    }
} // namespace

int runDaemon(DaemonConfig const& config)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    auto host = SessionHost { loop, makeShellPtyFactory(config.shell), config.settings };

    auto listener = bindDaemonEndpoint(loop, config.socketPath.string());
    if (!listener)
        return listener.error();
    auto server = MuxServer { loop, std::move(*listener), tmux::makeControlModeHandler(loop, host) };

    auto const nativePath = nativeSocketPath(config.socketPath).string();
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer = MuxServer { loop, std::move(*nativeListener), makeNativeHandler(loop, host) };
    // No imsg endpoint on Windows: SCM_RIGHTS fd passing does not exist here.

    auto servers = std::vector<MuxServer*> { &server, &nativeServer };

    // Opt-in native TCP listener (loopback by default), same as the POSIX path.
    auto nativeTcpServer = std::optional<MuxServer> {};
    if (config.nativeTcp)
    {
        auto tcpListener = net::listen(loop, config.nativeTcp->host, config.nativeTcp->port);
        if (!tcpListener)
        {
            std::println(
                stderr, "contour daemon: native TCP listen failed: {}", tcpListener.error().toString());
            return EXIT_FAILURE;
        }
        auto tls = makeNativeTcpTls(*config.nativeTcp);
        if (!tls)
            return EXIT_FAILURE;
        auto const boundPort = (*tcpListener)->localPort();
        nativeTcpServer.emplace(loop,
                                std::move(*tcpListener),
                                makeTlsNativeHandler(loop, host, std::move(tls), config.nativeTcp->token));
        servers.push_back(&*nativeTcpServer);
        std::println(
            stderr, "contour daemon: native TCP listener on {}:{}", config.nativeTcp->host, boundPort);
    }

    auto shutdown = std::function<void()> { [&] {
        loop.post([&] {
            for (auto* each: servers)
                each->close();
            loop.requestStop();
        });
    } };
    gConsoleShutdown = &shutdown;
    ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    std::println(
        stderr, "contour daemon: serving on {} (native: {})", config.socketPath.string(), nativePath);
    loop.blockOn(serveAll(servers));

    ::SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    gConsoleShutdown = nullptr;

    std::println(stderr, "contour daemon: shut down");
    return EXIT_SUCCESS;
}

namespace
{
    /// Puts the console into raw VT mode for the attach lifetime and restores
    /// the saved modes on destruction.
    class RawConsole
    {
      public:
        RawConsole(): _stdin(::GetStdHandle(STD_INPUT_HANDLE)), _stdout(::GetStdHandle(STD_OUTPUT_HANDLE))
        {
            if (::GetConsoleMode(_stdin, &_savedInput))
            {
                auto const raw =
                    (_savedInput
                     & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))
                    | ENABLE_VIRTUAL_TERMINAL_INPUT;
                ::SetConsoleMode(_stdin, raw);
            }
            if (::GetConsoleMode(_stdout, &_savedOutput))
                ::SetConsoleMode(
                    _stdout, _savedOutput | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        }

        ~RawConsole()
        {
            ::SetConsoleMode(_stdin, _savedInput);
            ::SetConsoleMode(_stdout, _savedOutput);
        }

        RawConsole(RawConsole const&) = delete;
        RawConsole& operator=(RawConsole const&) = delete;
        RawConsole(RawConsole&&) = delete;
        RawConsole& operator=(RawConsole&&) = delete;

      private:
        HANDLE _stdin;
        HANDLE _stdout;
        DWORD _savedInput = 0;
        DWORD _savedOutput = 0;
    };

    /// Proposes the local console's cell size as the daemon's client area.
    coro::Task<void> proposeConsoleSize(client::AttachClient* attach)
    {
        auto info = CONSOLE_SCREEN_BUFFER_INFO {};
        if (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &info))
        {
            auto const columns = static_cast<uint32_t>(info.srWindow.Right - info.srWindow.Left + 1);
            auto const lines = static_cast<uint32_t>(info.srWindow.Bottom - info.srWindow.Top + 1);
            if (columns > 0 && lines > 0)
                attach->requestResize(columns, lines);
        }
        co_return;
    }

    /// The attach flow on Windows: mirrors POSIX attachFlow, but console
    /// input cannot park on the socket reactor — a dedicated blocking-read
    /// thread posts the bytes onto the loop instead.
    coro::Task<int> attachFlowWin32(net::EventLoop* loop, AttachEndpoint endpoint)
    {
        auto const token = endpointToken(endpoint);
        auto socket = co_await connectAttach(loop, std::move(endpoint));
        if (!socket)
        {
            std::println(stderr, "contour attach: {}", socket.error());
            co_return EXIT_FAILURE;
        }

        auto attach = client::AttachClient { *loop, std::move(*socket), token };
        auto activeSession = std::uint64_t { 0 };
        // Drive the outer console with ScreenMirror (see the POSIX attachFlow note).
        auto mirror = client::ScreenMirror {};
        auto const writeOut = [](std::string const& bytes) {
            auto written = DWORD { 0 };
            ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE),
                        bytes.data(),
                        static_cast<DWORD>(bytes.size()),
                        &written,
                        nullptr);
        };
        wireMirrorHandlers(attach, mirror, activeSession, writeOut);

        auto const rawMode = RawConsole {};
        auto detached = std::atomic<bool> { false };

        // Console reads block; ReadFile on the input handle cannot join the socket
        // reactor, so a dedicated thread blocks in ReadFile and posts bytes onto the
        // loop. It captures frame locals (attach, activeSession, detached) and the
        // loop BY REFERENCE, so it MUST be joined before this frame dies — a detached
        // thread would wake on the next keypress and dereference freed state. The
        // ReaderGuard below sets the stop flag, aborts the in-flight ReadFile, and
        // joins at scope exit.
        auto reader = std::thread { [loop, &attach, &activeSession, &detached] {
            auto buffer = std::array<char, 512> {};
            while (!detached.load())
            {
                auto read = DWORD { 0 };
                if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE),
                                buffer.data(),
                                static_cast<DWORD>(buffer.size()),
                                &read,
                                nullptr)
                    || read == 0)
                    break;
                auto bytes = std::string { buffer.data(), read };
                if (bytes.contains('\x1c')) // Ctrl-\ detaches
                {
                    detached = true;
                    loop->post([&attach] { attach.detach(); });
                    break;
                }
                loop->post([&attach, &activeSession, moved = std::move(bytes)] {
                    if (activeSession != 0)
                        attach.sendInput(activeSession, moved);
                });
            }
        } };

        // Joins the reader before the captured locals and the loop are destroyed.
        // CancelSynchronousIo aborts a ReadFile already in flight; the retry closes
        // the race where the thread issues a ReadFile just after the stop flag is
        // set (the cancel is a no-op unless the thread is inside the syscall).
        struct ReaderGuard
        {
            std::thread& thread;
            std::atomic<bool>& stop;
            ~ReaderGuard()
            {
                stop.store(true);
                auto const handle = thread.native_handle();
                while (::WaitForSingleObject(handle, 50) == WAIT_TIMEOUT)
                    ::CancelSynchronousIo(handle);
                thread.join();
            }
        } readerGuard { reader, detached };

        co_await coro::whenAll(attach.run(), proposeConsoleSize(&attach));

        // Restore the outer console's persistent state before RawConsole hands the modes back.
        writeOut(mirror.detachRestore());
        std::println(stderr, "\ncontour attach: detached");
        co_return attach.versionMismatch() ? EXIT_FAILURE : EXIT_SUCCESS;
    }
} // namespace

int runAttach(AttachEndpoint const& endpoint)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    // connectAttach resolves the native socket beside the control socket (unix) or
    // dials TCP+TLS, per the endpoint.
    return loop.blockOn(attachFlowWin32(&loop, endpoint));
}

#endif // _WIN32

} // namespace muxserver
