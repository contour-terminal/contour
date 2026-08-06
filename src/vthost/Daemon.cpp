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

#include <vthost/Daemon.hpp>

#include <crispy/Environment.hpp>
#include <crispy/Utils.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.hpp>
#include <net/PollEventSource.hpp>
#include <net/Sockets.hpp>
#include <net/Tls.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vthost/Logging.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/tmux/ControlSession.hpp>
#include <vthost/tmux/ImsgServer.hpp>

#ifndef _WIN32
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
                errorLog()("cannot build an ephemeral TLS certificate: {}", context.error());
                return nullptr;
            }
            daemonLog()("native TCP is using an ephemeral self-signed certificate (TOFU)");
            return *context;
        }
        auto const cert = readFileToString(config.tlsCertPath);
        auto const key = readFileToString(config.tlsKeyPath);
        if (!cert || !key)
        {
            // One line each, naming the file: "the certificate or the key" leaves the reader
            // to guess, and the two are configured by different options.
            if (!cert)
                errorLog()("cannot read the TLS certificate '{}'", config.tlsCertPath);
            if (!key)
                errorLog()("cannot read the TLS key '{}'", config.tlsKeyPath);
            return nullptr;
        }
        auto context = net::makeTlsServerContext(*cert, *key);
        if (!context)
        {
            errorLog()("cannot build the TLS server context: {}", context.error());
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
        return [tls = std::move(tls), base = std::move(base)](ConnectionId id,
                                                              std::unique_ptr<net::ISocket> socket) {
            return base(std::move(id), tls->wrap(std::move(socket)));
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
            // Naming the path matters: all four unix endpoints funnel through here, so a bare
            // "address already in use" would not say WHICH socket is taken.
            errorLog()("cannot bind {}: {}", path, listener.error().toString());
            // The overwhelmingly common way a daemon start fails, and the one whose cause the
            // message above still does not act on: a second `contour daemon` on the same label.
            // Its liveness probe (net::listenUnix) also leaves an accept + immediate EOF in the
            // INCUMBENT's log, which reads like a client attaching and quitting — so without a
            // remedy here, the two halves of the story are two processes apart.
            if (listener.error().code == net::NetErrorCode::AddressInUse)
                errorLog()("a daemon is already serving this socket; attach to it with "
                           "'contour client', or start a separate one with --label NAME");
            return std::unexpected(EXIT_FAILURE);
        }
        daemonLog()("bound {}", path);
        return std::move(*listener);
    }

    /// The one shutdown action every trigger shares: stop accepting on every endpoint, then
    /// unwind the loop so each connection flow cancels.
    ///
    /// MUST run on the loop thread — @c EventLoop::requestStop is documented loop-thread-only. The
    /// signal (POSIX) and console-control (Windows) triggers get there by posting this; the
    /// last-session trigger is already on it and posts only to leave the observer fan-out (see
    /// LastSessionWatcher).
    ///
    /// This skips the connections' graceful epilogues by design (see the "Daemon lifetime" section
    /// of docs/internals/vthost.md); peers see a clean FIN rather than a goodbye.
    /// @param loop The loop to unwind.
    /// @param servers Every bound endpoint (not owned).
    /// @param reason What asked for the shutdown, for the single log line.
    void requestDaemonShutdown(net::EventLoop& loop,
                               std::vector<ConnectionAcceptor*> const& servers,
                               std::string_view reason)
    {
        daemonLog()("{}; closing {} listener(s)", reason, servers.size());
        for (auto* each: servers)
            each->close();
        loop.requestStop();
    }

    /// Arms @p slot with the watcher that ends the daemon together with its last session, when
    /// @p lifecycle asks for it. Shared by the POSIX and Windows runDaemon bodies.
    ///
    /// Emplaces rather than returning, because LastSessionWatcher holds a non-movable
    /// ScopedStreamSubscription. Nothing is constructed for a persistent daemon, so it registers no
    /// observer at all rather than one that would decline on every close for the process's life.
    /// @param slot The watcher's storage; left empty unless @p lifecycle is ExitWhenEmpty.
    /// @param lifecycle Whether this daemon outlives its last session.
    /// @param host The host whose session closes are observed.
    /// @param loop The loop the deferred shutdown is posted onto.
    /// @param servers Every bound endpoint, closed on shutdown (not owned; must outlive @p slot).
    void installLastSessionWatcher(std::optional<LastSessionWatcher>& slot,
                                   DaemonLifecycle lifecycle,
                                   SessionHost& host,
                                   net::EventLoop& loop,
                                   std::vector<ConnectionAcceptor*> const& servers)
    {
        if (lifecycle != DaemonLifecycle::ExitWhenEmpty)
            return;

        // Announced up front, because "why did my daemon vanish?" must be answerable from the
        // default log — this category is enabled without any --log.
        daemonLog()("this daemon terminates with its last hosted session");
        slot.emplace(host, loop, [&loop, &servers] {
            requestDaemonShutdown(loop, servers, "the last hosted session closed");
        });
    }

    /// Probes whether a daemon is accepting connections on the native socket at
    /// @p path. Only reachability is asked: the established transport is dropped
    /// again as soon as the connect returns.
    ///
    /// AF_UNIX is a Windows citizen too (10 1803+ / afunix.h), so this needs no
    /// platform split — net::connectUnix owns the per-OS detail (winsock
    /// initialization, the SOCKET/int handle split, the path-length check).
    /// @param loop The reactor driving the connect (not owned).
    /// @param path The native socket file to probe.
    /// @return True if a daemon accepted the connection.
    [[nodiscard]] bool daemonAccepts(net::EventLoop& loop, std::string const& path)
    {
        return loop.blockOn(net::connectUnix(&loop, path)).has_value();
    }

#ifndef _WIN32
    // The production halves of @ref resolveExecutablePath, needed only where exec does not search
    // PATH itself. CreateProcess does, so Windows never resolves anything.

    /// Whether @p path names a file this process may execute — kept out of the search so the search
    /// itself needs no filesystem, which is what makes it testable.
    /// @param path The candidate path.
    /// @return True when it is a regular file this process has execute permission for.
    [[nodiscard]] bool isExecutableFile(std::filesystem::path const& path)
    {
        auto ec = std::error_code {};
        return std::filesystem::is_regular_file(path, ec) && ::access(path.c_str(), X_OK) == 0;
    }

#endif

    /// Starts the daemon described by @p args detached from this process, so the client may exit
    /// without taking the daemon down with it.
    ///
    /// Takes the argv prebuilt (see daemonSpawnArgs) rather than composing it here: between fork and
    /// exec the child may only call async-signal-safe functions, and allocating an argv would not be
    /// one, so it has to exist already. It also gives the flag list ONE spelling for both platforms.
    /// @param args The full argv; args[0] is the binary to exec. Mutable because execv takes
    ///        `char* const*` and CreateProcess may write to its command-line buffer.
    /// @return False if the process could not be started.
    [[nodiscard]] bool spawnDetachedDaemon(std::vector<std::string>& args)
    {
#ifndef _WIN32
        // Resolved in the PARENT: the child may only call async-signal-safe functions between fork
        // and exec, and a PATH walk is not one. argv[0] itself stays as the caller named it — that
        // is what the daemon's `ps` line and its own commandLine() replay should show.
        // crispy::Environment, not getenv: this runs on a client that already has a live reactor
        // thread, and the snapshot exists precisely so an environment read is not racing it.
        auto const image = resolveExecutablePath(
            args[0], crispy::defaultEnvironment().get("PATH").value_or(""), isExecutableFile);

        auto argv = std::vector<char*> {};
        argv.reserve(args.size() + 1);
        for (auto& arg: args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);

        auto const pid = ::fork();
        if (pid < 0)
            return false;
        if (pid == 0)
        {
            // Detached means detached from the SESSION, not merely from the parent's lifetime —
            // this is the POSIX half of what Windows gets from DETACHED_PROCESS below. Without it
            // the daemon stays in the invoking shell's session and foreground process group, so
            // Ctrl-C in that terminal delivers SIGINT to the whole group and the daemon's own
            // handler shuts down every hosted session it exists to keep alive; closing the terminal
            // is worse, because SIGHUP is not among the signals runDaemon blocks and its default
            // action kills the process outright, leaving the socket file behind unbound.
            //
            // A session leader has no controlling terminal, which is also why the descriptors are
            // reopened on /dev/null: keeping the tty open would hold the shell's terminal from
            // being released and let the daemon write into a terminal that has moved on. Its
            // diagnostics go to --log-file, which the backgrounding path always supplies.
            //
            // Only async-signal-safe calls, as everything between fork and exec must be — setsid,
            // open, dup2 and close all are.
            ::setsid();
            if (auto const devNull = ::open("/dev/null", O_RDWR); devNull >= 0)
            {
                ::dup2(devNull, STDIN_FILENO);
                ::dup2(devNull, STDOUT_FILENO);
                ::dup2(devNull, STDERR_FILENO);
                if (devNull > STDERR_FILENO)
                    ::close(devNull);
            }
            ::execv(image.c_str(), argv.data());
            ::_exit(EXIT_FAILURE);
        }
        return true;
#else
        // CreateProcess parses its command line in place and may write to it, so the buffer must be
        // mutable. joinCommandLine quotes every argument, argv[0] included: either can hold spaces
        // (`C:\Program Files\...`), which an unquoted command line would split.
        auto commandLine = joinCommandLine(args);
        auto startupInfo = STARTUPINFOA {};
        startupInfo.cb = sizeof(startupInfo);
        auto processInfo = PROCESS_INFORMATION {};
        if (!::CreateProcessA(nullptr,
                              commandLine.data(),
                              nullptr,
                              nullptr,
                              FALSE,
                              DETACHED_PROCESS | CREATE_NO_WINDOW,
                              nullptr,
                              nullptr,
                              &startupInfo,
                              &processInfo))
            return false;
        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);
        return true;
#endif
    }

    /// Spawns @p args detached, then blocks until a daemon accepts on @p nativePath.
    ///
    /// Shared by the client's auto-spawn (@ref ensureDaemon) and `contour daemon --background`,
    /// because "it returned" must mean "it is serving" for both: a caller that gets its prompt
    /// back and only then discovers the daemon never bound has no way to tell that from a
    /// daemon that bound and died.
    /// @param loop The reactor driving each readiness probe (not owned).
    /// @param args The full argv; mutable because execv/CreateProcess want it so.
    /// @param nativePath The native endpoint probed for readiness.
    /// @param timeout How long to wait before giving up.
    /// @return EXIT_SUCCESS once it accepts, EXIT_FAILURE on spawn failure or timeout.
    [[nodiscard]] int spawnAndAwaitDaemon(net::EventLoop& loop,
                                          std::vector<std::string>& args,
                                          std::string const& nativePath,
                                          std::chrono::seconds timeout)
    {
        if (!spawnDetachedDaemon(args))
            return EXIT_FAILURE;

        // Poll until the socket accepts connections or the timeout elapses.
        // Back off gradually: a freshly spawned daemon needs a moment to bind,
        // but polling at a fixed 100ms wastes ~50 connect/close cycles over the
        // default 5s timeout. Doubling the sleep each iteration caps at 1s.
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        auto sleepMs = std::chrono::milliseconds { 50 };
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (daemonAccepts(loop, nativePath))
                return EXIT_SUCCESS;
            auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining < sleepMs)
                sleepMs = remaining;
            std::this_thread::sleep_for(sleepMs);
            sleepMs = std::min(sleepMs * 2, std::chrono::milliseconds { 1000 });
        }
        return EXIT_FAILURE; // timeout
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
    // The host we asked for is also the name the certificate must carry: a pinned CA proves who
    // signed, not who was signed for.
    auto tls = net::makeTlsClientContext(tcp.caPem, tcp.host);
    if (!tls)
        co_return std::unexpected(tls.error());
    auto encrypted = (*tls)->wrap(std::move(*socket));
    if (!encrypted)
        co_return std::unexpected(std::string { "TLS handshake setup failed" });
    co_return std::move(encrypted);
}

std::vector<std::string> daemonSpawnArgs(std::string_view daemonBinary,
                                         std::filesystem::path const& socketPath,
                                         DaemonSpawnOptions const& spawnOptions,
                                         DaemonLifecycle lifecycle)
{
    auto args = std::vector<std::string> {
        std::string { daemonBinary },
        "daemon",
        std::format("--socket={}", socketPath.string()),
    };
    // A daemon we spawned belongs to the client that spawned it, so it must not outlive the
    // sessions it was started for.
    if (lifecycle == DaemonLifecycle::ExitWhenEmpty)
        args.emplace_back("--exit-with-last-session");
    if (!spawnOptions.filter.empty())
        args.push_back(std::format("--log={}", spawnOptions.filter));
    if (!spawnOptions.logFile.empty())
        args.push_back(std::format("--log-file={}", spawnOptions.logFile));
    // The client's own configuration decides how its sessions emulate, so a daemon it
    // spawns must read the same file and profile — otherwise the client renders a
    // deeper scrollback than the daemon retains, and a resync truncates it.
    if (!spawnOptions.configPath.empty())
        args.push_back(std::format("--config={}", spawnOptions.configPath));
    if (!spawnOptions.profileName.empty())
        args.push_back(std::format("--profile={}", spawnOptions.profileName));
    args.push_back(std::format("--size-policy={}", nameOf(spawnOptions.sizePolicy)));
    return args;
}

std::string resolveExecutablePath(std::string_view program,
                                  std::string_view searchPath,
                                  std::function<bool(std::filesystem::path const&)> const& isExecutable)
{
    auto const named = std::filesystem::path { program };
    // A name carrying a directory is already a path — execvp's rule, and what keeps `./contour`
    // and `/usr/bin/contour` meaning exactly what they say rather than being searched for.
    if (program.empty() || named.has_parent_path())
        return std::string { program };

    for (auto const entry: std::views::split(searchPath, PathListSeparator))
    {
        auto const directory = std::string_view { entry };
        // POSIX reads an empty PATH entry as the current directory; deliberately skipped, because
        // resolving argv[0] against the cwd is the ambiguity this function exists to remove.
        if (directory.empty())
            continue;
        if (auto candidate = std::filesystem::path { directory } / named; isExecutable(candidate))
            return candidate.string();
    }
    return std::string { program };
}

namespace
{
    /// Appends @p arg to @p line quoted the way `CommandLineToArgvW` un-quotes.
    ///
    /// Backslashes are literal EXCEPT immediately before a `"`, where a run of n is read as n/2
    /// backslashes and the quote is escaped if n was odd. So a run that would land against the
    /// closing quote must be doubled, and an embedded quote needs its run doubled plus one more
    /// backslash. Wrapping the argument in quotes and hoping is how `--config=C:\dir\` came to end
    /// its own quoting and glue the remainder of the command line into itself.
    /// @param line The command line being built.
    /// @param arg The argument to append.
    void appendQuoted(std::string& line, std::string_view arg)
    {
        line += '"';
        auto backslashes = std::size_t { 0 };
        for (auto const ch: arg)
        {
            if (ch == '\\')
            {
                ++backslashes;
                continue;
            }
            // A run before a quote is doubled and the quote escaped; before anything else it is
            // literal and travels as-is.
            line.append(ch == '"' ? (backslashes * 2) + 1 : backslashes, '\\');
            backslashes = 0;
            line += ch;
        }
        // The closing quote is a quote too: double whatever run reached it.
        line.append(backslashes * 2, '\\');
        line += '"';
    }
} // namespace

std::string joinCommandLine(std::span<std::string const> args)
{
    auto line = std::string {};
    for (auto const& arg: args)
    {
        if (!line.empty())
            line += ' ';
        // Quote every argument whole, unconditionally: CommandLineToArgv strips the quotes, and
        // quoting only the ones that look like they need it is how argv[0] came to be left bare.
        appendQuoted(line, arg);
    }
    return line;
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

    auto host = SessionHost { loop,
                              makeShellPtyFactory(config.shell),
                              config.settings,
                              crispy::defaultEnvironment(),
                              /*startPumps=*/true,
                              config.sizePolicy };

    auto listener = bindDaemonEndpoint(loop, config.socketPath.string());
    if (!listener)
        return listener.error();
    auto server = ConnectionAcceptor {
        loop, "control", std::move(*listener), tmux::makeControlModeHandler(loop, host)
    };

    // The native cells+deltas protocol listens beside the control-mode socket.
    auto const nativePath = nativeSocketPath(config.socketPath).string();
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer =
        ConnectionAcceptor { loop, "native", std::move(*nativeListener), makeNativeHandler(loop, host) };

    // The imsg endpoint serves the REAL tmux client binary
    // (`tmux -S <socket>-tmux -C attach-session`).
    auto const imsgPath = tmuxSocketPath(config.socketPath).string();
    auto imsgListener = bindDaemonEndpoint(loop, imsgPath);
    if (!imsgListener)
        return imsgListener.error();
    auto imsgServer =
        ConnectionAcceptor { loop, "imsg", std::move(*imsgListener), tmux::makeTmuxImsgHandler(loop, host) };

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
        compatServer.emplace(
            loop, "tmux-compat", std::move(*compatListener), tmux::makeTmuxImsgHandler(loop, host));
        servers.push_back(&*compatServer);
        daemonLog()("tmux-compat socket bound at {}", compatPath);
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
            errorLog()("cannot listen on {}:{}: {}",
                       config.nativeTcp->host,
                       config.nativeTcp->port,
                       tcpListener.error().toString());
            return EXIT_FAILURE;
        }
        auto tls = makeNativeTcpTls(*config.nativeTcp);
        if (!tls)
            return EXIT_FAILURE;
        auto const boundPort = (*tcpListener)->localPort();
        nativeTcpServer.emplace(loop,
                                "native-tcp",
                                std::move(*tcpListener),
                                makeTlsNativeHandler(loop, host, std::move(tls), config.nativeTcp->token));
        servers.push_back(&*nativeTcpServer);
        daemonLog()("native TCP listener on {}:{}", config.nativeTcp->host, boundPort);
    }

    // An auto-spawned daemon ends with its last session; a user-started one persists. Declared after
    // `servers` is complete (it is captured by reference) and after `host`, so it unsubscribes while
    // the host is still alive.
    auto lastSession = std::optional<LastSessionWatcher> {};
    installLastSessionWatcher(lastSession, config.lifecycle, host, loop, servers);

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
        // Everything below runs on the LOOP thread, deliberately: this lambda body is the
        // sigwait thread, and logging from there would put a line outside the single-threaded
        // invariant the rest of the module keeps.
        loop.post([&, sig] { requestDaemonShutdown(loop, servers, std::format("signal {} received", sig)); });
    } };

    daemonLog()("serving on {} (native: {}, tmux: {})", config.socketPath.string(), nativePath, imsgPath);
    loop.blockOn(serveAll(servers));

    // Unblock the sigwait thread when the shutdown came from elsewhere — the last-session
    // exit, for which no signal was ever delivered. If it already consumed a signal, this
    // one stays blocked and dies with the process.
    //
    // raise(3) targets the calling thread (pthread_kill(pthread_self(), …)) — the watcher
    // thread's sigwait dequeues from the PROCESS queue and never sees a thread-directed
    // signal, so the join below would hang. kill(getpid(), …) is process-directed.
    if (!signalSeen)
        std::ignore = ::kill(::getpid(), SIGTERM);
    watcher.join();

    daemonLog()("shut down");
    return EXIT_SUCCESS;
}

#endif // _WIN32

int ensureDaemon(AttachEndpoint const& endpoint,
                 std::string_view daemonBinary,
                 DaemonSpawnOptions const& spawnOptions,
                 std::chrono::seconds timeout)
{
    // TCP endpoints: no auto-spawn (can't spawn a remote daemon).
    if (std::holds_alternative<TcpEndpoint>(endpoint))
        return EXIT_SUCCESS;

    // Unix socket: probe the native socket beside the control socket. One reactor
    // serves the initial probe and every poll that follows.
    auto const& unixEp = std::get<UnixEndpoint>(endpoint);
    auto const nativePath = nativeSocketPath(unixEp.socketPath).string();

    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    if (daemonAccepts(loop, nativePath))
        return EXIT_SUCCESS; // daemon is already running

    // Daemon not running — spawn it in the background. A daemon we spawn is ours, so it is started
    // `--exit-with-last-session` and ends when the work it was spawned for is done.
    auto args =
        daemonSpawnArgs(daemonBinary, unixEp.socketPath, spawnOptions, DaemonLifecycle::ExitWhenEmpty);
    return spawnAndAwaitDaemon(loop, args, nativePath, timeout);
}

int runDaemonDetached(std::vector<std::string> commandLine,
                      std::filesystem::path const& socketPath,
                      std::chrono::seconds timeout)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    return spawnAndAwaitDaemon(loop, commandLine, nativeSocketPath(socketPath).string(), timeout);
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

    auto host = SessionHost { loop,
                              makeShellPtyFactory(config.shell),
                              config.settings,
                              crispy::defaultEnvironment(),
                              /*startPumps=*/true,
                              config.sizePolicy };

    auto listener = bindDaemonEndpoint(loop, config.socketPath.string());
    if (!listener)
        return listener.error();
    auto server = ConnectionAcceptor {
        loop, "control", std::move(*listener), tmux::makeControlModeHandler(loop, host)
    };

    auto const nativePath = nativeSocketPath(config.socketPath).string();
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer =
        ConnectionAcceptor { loop, "native", std::move(*nativeListener), makeNativeHandler(loop, host) };
    // No imsg endpoint on Windows: SCM_RIGHTS fd passing does not exist here.

    auto servers = std::vector<ConnectionAcceptor*> { &server, &nativeServer };

    // Opt-in native TCP listener (loopback by default), same as the POSIX path.
    auto nativeTcpServer = std::optional<ConnectionAcceptor> {};
    if (config.nativeTcp)
    {
        auto tcpListener = net::listen(loop, config.nativeTcp->host, config.nativeTcp->port);
        if (!tcpListener)
        {
            errorLog()("cannot listen on {}:{}: {}",
                       config.nativeTcp->host,
                       config.nativeTcp->port,
                       tcpListener.error().toString());
            return EXIT_FAILURE;
        }
        auto tls = makeNativeTcpTls(*config.nativeTcp);
        if (!tls)
            return EXIT_FAILURE;
        auto const boundPort = (*tcpListener)->localPort();
        nativeTcpServer.emplace(loop,
                                "native-tcp",
                                std::move(*tcpListener),
                                makeTlsNativeHandler(loop, host, std::move(tls), config.nativeTcp->token));
        servers.push_back(&*nativeTcpServer);
        daemonLog()("native TCP listener on {}:{}", config.nativeTcp->host, boundPort);
    }

    // An auto-spawned daemon ends with its last session; a user-started one persists. See the
    // POSIX body above for why this is declared here.
    auto lastSession = std::optional<LastSessionWatcher> {};
    installLastSessionWatcher(lastSession, config.lifecycle, host, loop, servers);

    auto shutdown = std::function<void()> { [&] {
        // The console-control handler runs on an arbitrary OS thread; logging happens inside
        // the POSTED body so it lands on the loop thread like everything else.
        loop.post([&] { requestDaemonShutdown(loop, servers, "console shutdown requested"); });
    } };
    consoleShutdown = &shutdown;
    ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    daemonLog()("serving on {} (native: {})", config.socketPath.string(), nativePath);
    loop.blockOn(serveAll(servers));

    ::SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    consoleShutdown = nullptr;

    daemonLog()("shut down");
    return EXIT_SUCCESS;
}

#endif // _WIN32

} // namespace vthost
