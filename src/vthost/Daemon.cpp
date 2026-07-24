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

#include <vthost/Daemon.h>

#include <crispy/algorithm.h>
#include <crispy/utils.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <net/Tls.h>
#include <vthost/ConnectionAcceptor.h>
#include <vthost/NativeSession.h>
#include <vthost/SessionHost.h>
#include <vthost/SocketPath.h>
#include <vthost/tmux/ControlSession.h>
#include <vthost/tmux/ImsgServer.h>

#ifndef _WIN32
    #include <sys/socket.h>
    #include <sys/un.h>

    #include <csignal>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vthost
{

namespace
{
    /// Drives every protocol server's accept loop on the one reactor.
    coro::Task<void> serveAll(std::vector<ConnectionAcceptor*> servers)
    {
        auto accepts = std::vector<coro::Task<void>> {};
        accepts.reserve(servers.size());
        for (auto* server: servers)
            accepts.push_back(server->serve());
        co_await coro::whenAll(std::move(accepts));
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
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return std::nullopt;
        return crispy::readFileAsString(path);
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
    auto server = ConnectionAcceptor { loop, std::move(*listener), tmux::makeControlModeHandler(loop, host) };

    // The native cells+deltas protocol listens beside the control-mode socket.
    auto const nativePath = nativeSocketPath(config.socketPath).string();
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer =
        ConnectionAcceptor { loop, std::move(*nativeListener), makeNativeHandler(loop, host) };

    // The imsg endpoint serves the REAL tmux client binary
    // (`tmux -S <socket>-tmux -C attach-session`).
    auto const imsgPath = tmuxSocketPath(config.socketPath).string();
    auto imsgListener = bindDaemonEndpoint(loop, imsgPath);
    if (!imsgListener)
        return imsgListener.error();
    auto imsgServer =
        ConnectionAcceptor { loop, std::move(*imsgListener), tmux::makeTmuxImsgHandler(loop, host) };

    auto servers = std::vector<ConnectionAcceptor*> { &server, &nativeServer, &imsgServer };

    // Opt-in: ALSO bind tmux's own discovery path, so a plain
    // `tmux -L <label> -C attach-session` finds this daemon. Opt-in only —
    // with the daemon down, a `new-session` on that path silently forks a
    // REAL tmux server onto it.
    auto compatServer = std::optional<ConnectionAcceptor> {};
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
    auto nativeTcpServer = std::optional<ConnectionAcceptor> {};
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

#endif // _WIN32

int ensureDaemon(AttachEndpoint const& endpoint, std::string_view daemonBinary, std::chrono::seconds timeout)
{
    // TCP endpoints: no auto-spawn (can't spawn a remote daemon).
    if (std::holds_alternative<TcpEndpoint>(endpoint))
        return EXIT_SUCCESS;

    // Unix socket: probe the native socket beside the control socket.
    auto const& unixEp = std::get<UnixEndpoint>(endpoint);
    auto const nativePath = nativeSocketPath(unixEp.socketPath);

    // Quick probe: try a non-blocking connect. If it succeeds, the daemon is alive.
    auto probeSock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probeSock < 0)
        return EXIT_FAILURE;
#ifndef _WIN32
    ::fcntl(probeSock, F_SETFD, FD_CLOEXEC);
#endif
    auto addr = sockaddr_un {};
    addr.sun_family = AF_UNIX;
    auto const pathLen = nativePath.string().copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[pathLen] = '\0';
    if (::connect(probeSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
    {
#ifdef _WIN32
        ::closesocket(probeSock);
#else
        ::close(probeSock);
#endif
        return EXIT_SUCCESS; // daemon is already running
    }
#ifdef _WIN32
    ::closesocket(probeSock);
#else
    ::close(probeSock);
#endif

    // Daemon not running — spawn it in the background.
    auto const deadline = std::chrono::steady_clock::now() + timeout;
#ifndef _WIN32
    auto const pid = ::fork();
    if (pid == 0)
    {
        // Child: exec contour daemon with the same socket path.
        auto const socketArg = std::format("--socket={}", unixEp.socketPath.string());
        auto const daemonBinaryStr = std::string(daemonBinary);
        ::execl(daemonBinaryStr.c_str(), daemonBinaryStr.c_str(), "daemon", socketArg.c_str(), nullptr);
        ::_exit(EXIT_FAILURE);
    }
#else
    // Windows: CreateProcess with DETACHED_PROCESS, then poll.
    auto const cmdLine = std::format("{} daemon --socket=\"{}\"", daemonBinary, unixEp.socketPath.string());
    auto si = STARTUPINFOA {};
    si.cb = sizeof(si);
    auto pi = PROCESS_INFORMATION {};
    if (!::CreateProcessA(nullptr,
                          cmdLine.data(),
                          nullptr,
                          nullptr,
                          FALSE,
                          DETACHED_PROCESS | CREATE_NO_WINDOW,
                          nullptr,
                          nullptr,
                          &si,
                          &pi))
        return EXIT_FAILURE;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
#endif
    // Parent: poll until the socket accepts connections or timeout.
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto pollSock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (pollSock >= 0)
        {
#ifndef _WIN32
            ::fcntl(pollSock, F_SETFD, FD_CLOEXEC);
#endif
            if (::connect(pollSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            {
#ifdef _WIN32
                ::closesocket(pollSock);
#else
                ::close(pollSock);
#endif
                return EXIT_SUCCESS;
            }
#ifdef _WIN32
            ::closesocket(pollSock);
#else
            ::close(pollSock);
#endif
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return EXIT_FAILURE; // timeout
}

#ifdef _WIN32

namespace
{
    /// The console-control shutdown hook: SetConsoleCtrlHandler's callback is
    /// a plain C function invoked on an arbitrary thread with no user data
    /// pointer, so the running daemon registers its (thread-safe) shutdown
    /// poster in this one process-wide slot.
    std::atomic<std::function<void()>*> consoleShutdown = nullptr;

    BOOL WINAPI consoleCtrlHandler(DWORD type)
    {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
            return FALSE;
        if (auto* shutdown = consoleShutdown.load())
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
    auto server = ConnectionAcceptor { loop, std::move(*listener), tmux::makeControlModeHandler(loop, host) };

    auto const nativePath = nativeSocketPath(config.socketPath).string();
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer =
        ConnectionAcceptor { loop, std::move(*nativeListener), makeNativeHandler(loop, host) };
    // No imsg endpoint on Windows: SCM_RIGHTS fd passing does not exist here.

    auto servers = std::vector<ConnectionAcceptor*> { &server, &nativeServer };

    // Opt-in native TCP listener (loopback by default), same as the POSIX path.
    auto nativeTcpServer = std::optional<ConnectionAcceptor> {};
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
    consoleShutdown = &shutdown;
    ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    std::println(
        stderr, "contour daemon: serving on {} (native: {})", config.socketPath.string(), nativePath);
    loop.blockOn(serveAll(servers));

    ::SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    consoleShutdown = nullptr;

    std::println(stderr, "contour daemon: shut down");
    return EXIT_SUCCESS;
}

#endif // _WIN32

} // namespace vthost
