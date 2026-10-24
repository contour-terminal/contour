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

#include <vtbackend/core/TerminalContext.hpp>

#include <vtpty/DetachedProcess.hpp>
#include <vtpty/ProgramLookup.hpp>
#include <vtpty/WindowsCommandLine.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/WideString.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/SystemInfo.hpp>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/DaemonRuntime.hpp>
#include <vthost/HostedShell.hpp>
#include <vthost/IdleExitWatcher.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vthost/Logging.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/ShutdownFunnel.hpp>
#include <vthost/ShutdownSignals.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/tmux/ControlSession.hpp>
#include <vthost/tmux/ImsgServer.hpp>

#ifndef _WIN32
    #include <core/net/posix/UnixListener.hpp>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vthost
{

namespace
{
    /// The machine this daemon runs on, read once from the OS for every session it will host.
    [[nodiscard]] vtbackend::HostIdentity thisHostIdentity()
    {
        return vtbackend::readHostIdentity(core::platform::NativeFileSystem::instance(),
                                           core::platform::hostName());
    }

    /// Drives every protocol server's accept loop on the one reactor.
    core::async::Task<void> serveAll(std::vector<ConnectionAcceptor*> servers)
    {
        auto accepts = std::vector<core::async::Task<void>> {};
        accepts.reserve(servers.size());
        for (auto* server: servers)
            accepts.push_back(server->serve());
        co_await core::async::whenAll(std::move(accepts));
    }

    /// The daemon's PTY factory: every session spawns the configured shell over a
    /// fresh PTY. Shared by the POSIX and Windows runDaemon paths.
    ///
    /// @param shell         What to spawn; a spawn's override (a layout pane's command and directory) is
    ///                      laid over it. A host with profiles spawns through makeProfilePtyFactory.
    /// @param escapeSandbox Whether the spawned shell escapes its sandbox (Flatpak), from the
    ///                      resolved profile's `escape_sandbox`. Was hardcoded `true` before this
    ///                      parameter existed, silently ignoring a profile's `escape_sandbox: false`.
    /// @param socketPath    The daemon's control socket, which every hosted shell is told about so
    ///                      it can reach back. Merged in here rather than by the caller, so that
    ///                      every entry into runDaemon() hosts shells that can find their own daemon.
    /// @param placement     Moves each hosted shell into its own resource domain, so an
    ///                      out-of-memory kill inside one cannot stop the daemon's unit.
    [[nodiscard]] PtyFactory makeShellPtyFactory(vtpty::Process::ExecInfo shell,
                                                 bool escapeSandbox,
                                                 std::filesystem::path const& socketPath,
                                                 std::shared_ptr<vtpty::ProcessPlacement> placement)
    {
        return
            [shell = std::move(shell),
             hosted = hostedShellEnvironment(socketPath),
             escapeSandbox,
             placement = std::move(placement)](vtbackend::PageSize pageSize,
                                               std::optional<vtpty::Process::ExecInfo> const& commandOverride)
                -> std::unique_ptr<vtpty::Pty> {
                // A fresh shell per spawn: `shell` is captured by the closure and reused for every session
                // this factory spawns, so writing one session's override back into it would leak that pane's
                // command/directory into every later session. Mirrors AppSessionFactory::createPty.
                return std::make_unique<vtpty::Process>(
                    hostedShellFor(shell, commandOverride, ShellOverride::Overlays, hosted),
                    vtpty::createPty(pageSize, std::nullopt),
                    escapeSandbox,
                    placement);
            };
    }

    /// The PTY factory of a daemon with profiles: each session runs its resolved profile's whole shell, with
    /// that profile's own environment and sandbox policy, as the in-process GUI spawns that profile
    /// (SessionFactory takes `escape_sandbox` from the session's own profile).
    /// @param shell      The daemon's default shell, whose program runs when a profile names none.
    /// @param socketPath The daemon's control socket, which every hosted shell is told about.
    /// @param placement  Moves each hosted shell into its own resource domain.
    [[nodiscard]] ProfilePtyFactory makeProfilePtyFactory(vtpty::Process::ExecInfo shell,
                                                          std::filesystem::path const& socketPath,
                                                          std::shared_ptr<vtpty::ProcessPlacement> placement)
    {
        return [base = std::move(shell),
                hosted = hostedShellEnvironment(socketPath),
                placement = std::move(placement)](vtbackend::PageSize pageSize,
                                                  vtpty::Process::ExecInfo const& profileShell,
                                                  SandboxEscape sandbox) -> std::unique_ptr<vtpty::Pty> {
            return std::make_unique<vtpty::Process>(
                hostedShellFor(base, profileShell, ShellOverride::Replaces, hosted),
                vtpty::createPty(pageSize, std::nullopt),
                sandbox == SandboxEscape::Escapes,
                placement);
        };
    }

    /// Reads the whole file at @p path, or nullopt if it cannot be opened.
    /// @param path The file, in UTF-8.
    [[nodiscard]] std::optional<std::string> readFileToString(std::string const& path)
    {
        auto const file = core::pathFromUtf8(path);
        std::error_code ec;
        if (!file || !std::filesystem::exists(*file, ec))
            return std::nullopt;
        return core::readFileAsString(*file);
    }

    /// Builds the TLS server context for the native TCP listener: an ephemeral
    /// self-signed certificate when none is configured (the TOFU default), else
    /// the configured PEM cert + key. The TCP transport is always encrypted.
    /// @return The context, or nullptr after printing why it could not be built.
    [[nodiscard]] std::shared_ptr<core::net::ITlsContext> makeNativeTcpTls(
        NativeTcpListenerConfig const& config)
    {
        if (config.tlsCertPath.empty() && config.tlsKeyPath.empty())
        {
            auto context = core::net::makeSelfSignedServerContext({ .commonName = "contour-daemon" });
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
        auto context = core::net::makeTlsServerContext(*cert, *key);
        if (!context)
        {
            errorLog()("cannot build the TLS server context: {}", context.error());
            return nullptr;
        }
        return *context;
    }

    /// Composes a native connection handler that FIRST encrypts each accepted
    /// socket (server-side TLS), then serves the native protocol over it.
    /// @param services What each native connection is served with, the TCP token among them.
    [[nodiscard]] ConnectionHandler makeTlsNativeHandler(core::net::EventLoop& loop,
                                                         SessionHost& host,
                                                         std::shared_ptr<core::net::ITlsContext> tls,
                                                         NativeSessionServices services)
    {
        auto base = makeNativeHandler(loop, host, std::move(services));
        return [tls = std::move(tls), base = std::move(base), &loop](
                   ConnectionId id, std::unique_ptr<core::net::ISocket> socket) {
            return base(std::move(id), tls->wrap(std::move(socket), loop));
        };
    }

    /// Binds a unix listener at @p path, or prints the error and yields the process
    /// exit code the caller should return. Collapses the bind/report/return block
    /// every daemon endpoint otherwise repeats verbatim.
    [[nodiscard]] std::expected<std::unique_ptr<core::net::IListener>, int> bindDaemonEndpoint(
        core::net::EventLoop& loop, std::string const& path)
    {
        auto listener = core::net::listenUnix(loop, path);
        if (!listener)
        {
            // Naming the path matters: all four unix endpoints funnel through here, so a bare
            // "address already in use" would not say WHICH socket is taken.
            errorLog()("cannot bind {}: {}", path, listener.error().toString());
            // The overwhelmingly common way a daemon start fails, and the one whose cause the
            // message above still does not act on: a second `contour daemon` on the same label.
            // Its liveness probe (core::net::listenUnix) also leaves an accept + immediate EOF in the
            // INCUMBENT's log, which reads like a client attaching and quitting — so without a
            // remedy here, the two halves of the story are two processes apart.
            if (listener.error().code == core::net::NetErrorCode::AddressInUse)
                errorLog()("a daemon is already serving this socket; attach to it with "
                           "'contour client', or start a separate one with --label NAME");
            return std::unexpected(EXIT_FAILURE);
        }
        daemonLog()("bound {}", path);
        return std::move(*listener);
    }

    /// The watcher a daemon's lifecycle calls for, if any. Holds at most one, because a lifecycle is one
    /// rule; the watchers hold non-movable subscriptions, so they are emplaced rather than returned.
    struct LifecycleWatchers
    {
        std::optional<LastSessionWatcher> lastSession; ///< Armed for ExitWhenEmpty.
        std::optional<IdleExitWatcher> idle;           ///< Armed for ExitWhenIdle.
    };

    /// Arms the watcher that ends the daemon on its own, when @p config's lifecycle asks for one. Shared
    /// by the POSIX and Windows runDaemon bodies.
    ///
    /// Nothing is constructed for a Persistent daemon, so it registers no observer at all rather than one
    /// that would decline for the process's life.
    /// @param watchers The watchers' storage; left empty for a Persistent daemon.
    /// @param config The daemon's configuration: its lifecycle and idle grace.
    /// @param host The host whose sessions are observed.
    /// @param census The daemon's client count.
    /// @param loop The loop the deferred shutdown is posted onto.
    /// @param funnel The daemon's shutdown (not owned; must outlive @p watchers).
    void installLifecycleWatchers(LifecycleWatchers& watchers,
                                  DaemonConfig const& config,
                                  SessionHost& host,
                                  ClientCensus& census,
                                  core::net::EventLoop& loop,
                                  ShutdownFunnel& funnel)
    {
        // Announced up front, because "why did my daemon vanish?" must be answerable from the
        // default log — this category is enabled without any --log.
        switch (config.lifecycle)
        {
            case DaemonLifecycle::Persistent: return;
            case DaemonLifecycle::ExitWhenEmpty:
                daemonLog()("this daemon terminates with its last hosted session");
                watchers.lastSession.emplace(
                    host, loop, [&funnel] { funnel.request(ShutdownCause::LastSessionClosed); });
                return;
            case DaemonLifecycle::ExitWhenIdle:
                daemonLog()("this daemon terminates after {} s with no session and no client",
                            config.idleExitGrace.count());
                watchers.idle.emplace(host, census, loop, config.idleExitGrace, [&funnel] {
                    funnel.request(ShutdownCause::Idle);
                });
                return;
        }
    }

    /// A shutdown request that may be made from anywhere — another thread, or a connection's PDU
    /// handler — because it only posts @p funnel's request onto @p loop, the loop's one thread-safe entry
    /// point. A StopDaemon is answered inside NativeSession's pump handler, which the shutdown would
    /// unwind, and like LastSessionWatcher's this gives each write queue one pump to flush.
    /// @param loop The loop the shutdown runs on (not owned).
    /// @param funnel The daemon's shutdown (not owned; outlives every request, see runDaemon).
    /// @return The request.
    [[nodiscard]] std::function<void(ShutdownCause)> postedShutdown(core::net::EventLoop& loop,
                                                                    ShutdownFunnel& funnel)
    {
        return [&loop, &funnel](ShutdownCause cause) {
            loop.post([&funnel, cause] { funnel.request(cause); });
        };
    }

    /// @param config The daemon's configuration.
    /// @return What its DaemonRuntime states about it.
    [[nodiscard]] DaemonRuntimeConfig runtimeConfigOf(DaemonConfig const& config)
    {
        return DaemonRuntimeConfig { .releaseVersion = config.releaseVersion,
                                     .processId = currentProcessId(),
                                     .socketPath = config.socketPath,
                                     .executablePath = config.executablePath,
                                     .logFile = config.logFile,
                                     .lifecycle = config.lifecycle };
    }

    /// What every native endpoint of a daemon serves its connections with: @p runtime behind them, and a
    /// handshake that offers DaemonControl, because @p runtime answers it. Only a daemon offers it; an
    /// embedded host's default handshake keeps not claiming what its null daemon control refuses.
    /// @param runtime The daemon (not owned; outlives every connection).
    /// @param config The daemon's configuration: with profiles, the handshake offers ProfileByName too.
    /// @param token The preshared token required of every client; empty for the unix endpoint.
    /// @return The services.
    [[nodiscard]] NativeSessionServices daemonNativeServices(DaemonRuntime& runtime,
                                                             DaemonConfig const& config,
                                                             std::string token = {})
    {
        auto capabilities = proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions,
                                                       proto::Capability::DaemonControl,
                                                       proto::Capability::InitialClientArea,
                                                       proto::Capability::AttachIntent,
                                                       proto::Capability::TabEdits,
                                                       proto::Capability::Heartbeat });
        // Only a daemon with profiles resolves a name; one without keeps honouring a client's settings.
        if (config.profiles)
            capabilities.idents.push_back(std::to_underlying(proto::Capability::ProfileByName));
        return NativeSessionServices {
            .handshake = { .expectedToken = std::move(token), .capabilities = std::move(capabilities) },
            .daemon = runtime,
        };
    }

    /// Probes whether a daemon is accepting connections on the native socket at
    /// @p path. Only reachability is asked: the established transport is dropped
    /// again as soon as the connect returns.
    ///
    /// AF_UNIX is a Windows citizen too (10 1803+ / afunix.h), so this needs no
    /// platform split — core::net::connectUnix owns the per-OS detail (winsock
    /// initialization, the SOCKET/int handle split, the path-length check).
    /// @param loop The reactor driving the connect (not owned).
    /// @param path The native socket file to probe.
    /// @return True if a daemon accepted the connection.
    [[nodiscard]] bool daemonAccepts(core::net::EventLoop& loop, std::string const& path)
    {
        return loop.blockOn(core::net::connectUnix(&loop, path)).has_value();
    }

    /// Starts the daemon described by @p args detached from this process, so the client may exit
    /// without taking the daemon down with it.
    ///
    /// Takes the argv prebuilt (see daemonSpawnArgs) rather than composing it here. It gives the flag
    /// list ONE spelling for both platforms.
    /// @param args The full argv; args[0] is the binary to exec.
    /// @param placement Places the daemon before it runs, as a session's shell is placed. POSIX only.
    /// @return Nothing, or why the process could not be started.
    [[nodiscard]] std::expected<void, std::string> spawnDetachedDaemon(
        std::span<std::string const> args, [[maybe_unused]] vtpty::ProcessPlacement& placement)
    {
        if (args.empty())
            return std::unexpected("no program to start");
#ifndef _WIN32
        // Resolved here, not in the child, which may only call async-signal-safe functions before it
        // execs: a PATH walk is not one. argv[0] itself stays as the caller named it -- that is what
        // the daemon's `ps` line and its own commandLine() replay should show.
        // core::Environment, not getenv: this runs on a client that already has a live reactor
        // thread, and the snapshot exists precisely so an environment read is not racing it.
        auto image = vtpty::resolveExecutablePath(args[0],
                                                  core::defaultEnvironment().get("PATH").value_or(""),
                                                  core::platform::NativeFileSystem::instance());

        // Detached means detached from the SESSION, not merely from the parent's lifetime -- this is
        // the POSIX half of what Windows gets from DETACHED_PROCESS below. Without it the daemon stays
        // in the invoking shell's session and foreground process group, so Ctrl-C in that terminal
        // delivers SIGINT to the whole group and the daemon's own handler shuts down every hosted
        // session it exists to keep alive; closing the terminal is worse, because SIGHUP is not among
        // the signals runDaemon blocks and its default action kills the process outright, leaving the
        // socket file behind unbound. startDetached() gives it a session of its own.
        //
        // Its standard streams are /dev/null: holding the shell's terminal open would keep it from
        // being released, and let the daemon write into a terminal that has moved on. Its
        // diagnostics go to --log-file, which the backgrounding path always supplies.
        //
        // Placed, as a session's shell is: the daemon holds the scrollback of every session it hosts,
        // and an out-of-memory kill elsewhere in the client's unit, or in the tab it was typed into,
        // must not end it.
        return vtpty::startDetached(
                   vtpty::DetachedCommand { .image = std::move(image),
                                            .argv = std::vector<std::string>(args.begin(), args.end()),
                                            .standardInput = "/dev/null",
                                            .output = vtpty::DetachedOutput::Discarded,
                                            .role = vtpty::ProcessRole::Daemon },
                   placement)
            .transform([](int) {})
            .transform_error([](std::error_code const& error) { return error.message(); });
#else
        // CreateProcess parses its command line in place and may write to it, so the buffer must be
        // mutable. joinCommandLine quotes an argument, argv[0] included, where it holds a space
        // (`C:\Program Files\...`), which an unquoted command line would split.
        auto commandLine = core::toUtf16(joinCommandLine(args));
        if (!commandLine)
            return std::unexpected("the command line is not valid UTF-8");
        auto startupInfo = STARTUPINFOW {};
        startupInfo.cb = sizeof(startupInfo);
        auto processInfo = PROCESS_INFORMATION {};
        if (!::CreateProcessW(nullptr,
                              commandLine->data(),
                              nullptr,
                              nullptr,
                              FALSE,
                              DETACHED_PROCESS | CREATE_NO_WINDOW,
                              nullptr,
                              nullptr,
                              &startupInfo,
                              &processInfo))
            return std::unexpected(std::system_category().message(static_cast<int>(::GetLastError())));
        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);
        return {};
#endif
    }

    /// The operating system's DaemonSpawner: one reactor serves every probe it is asked.
    class NativeDaemonSpawner final: public DaemonSpawner
    {
      public:
        NativeDaemonSpawner(): _source { core::net::makeDefaultBackend() }, _loop { *_source } {}

        [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                             vtpty::ProcessPlacement& placement) override
        {
            return spawnDetachedDaemon(argv, placement);
        }

        [[nodiscard]] DaemonPresence probe(std::filesystem::path const& socketPath) override
        {
            return daemonAccepts(_loop, core::pathToUtf8(nativeSocketPath(socketPath)))
                       ? DaemonPresence::Serving
                       : DaemonPresence::Absent;
        }

      private:
        std::unique_ptr<core::net::IoBackend> _source;
        core::net::EventLoop _loop;
    };

    /// Spawns @p args detached, then blocks until a daemon serves at @p socketPath.
    ///
    /// Shared by the client's auto-spawn (@ref ensureDaemon) and `contour daemon --background`,
    /// because "it returned" must mean "it is serving" for both: a caller that gets its prompt
    /// back and only then discovers the daemon never bound has no way to tell that from a
    /// daemon that bound and died.
    /// @param spawner What starts the daemon and asks whether it serves.
    /// @param args The full argv.
    /// @param placement Places the daemon before it runs.
    /// @param socketPath The control socket the daemon binds.
    /// @param timeout How long to wait before giving up.
    /// @return EXIT_SUCCESS once it accepts, EXIT_FAILURE on spawn failure or timeout.
    [[nodiscard]] int spawnAndAwaitDaemon(DaemonSpawner& spawner,
                                          std::span<std::string const> args,
                                          vtpty::ProcessPlacement& placement,
                                          std::filesystem::path const& socketPath,
                                          std::chrono::milliseconds timeout)
    {
        if (auto const spawned = spawner.spawn(args, placement); !spawned)
        {
            daemonLog()("cannot start {}: {}", args.empty() ? std::string {} : args.front(), spawned.error());
            return EXIT_FAILURE;
        }

        return awaitDaemon(spawner, socketPath, timeout) == DaemonPresence::Serving ? EXIT_SUCCESS
                                                                                    : EXIT_FAILURE;
    }
} // namespace

uint64_t drawDaemonInstance()
{
    auto device = std::random_device {};
    auto engine = std::mt19937_64 { (uint64_t { device() } << 32U) ^ device() };
    auto draw = std::uniform_int_distribution<uint64_t> { 1, std::numeric_limits<uint64_t>::max() };
    return draw(engine);
}

std::expected<void, std::string> prepareSocketDirectory(
    [[maybe_unused]] std::filesystem::path const& socketPath)
{
#ifndef _WIN32
    auto const directory = socketPath.parent_path();
    if (directory.empty())
        return {};
    return core::net::ensureOwnedPrivateDirectory(directory).transform_error(
        [](core::net::NetError const& error) { return error.toString(); });
#else
    return {};
#endif
}

namespace
{
    /// @param quotedTail What follows an argument's opening quote.
    /// @return The argument up to its closing quote, with systemd's escapes undone: `\\` and `\"`, `%%`
    ///         and `$$`. A backslash before any other character is itself, as on a Windows command line.
    [[nodiscard]] std::string unquoteArgument(std::string_view quotedTail)
    {
        auto decoded = std::string {};
        // A `\`, `%` or `$` read but not yet written: what it means depends on the character after it.
        auto pending = char {};
        for (auto const ch: quotedTail)
        {
            if (pending != char {})
            {
                auto const escape = std::exchange(pending, char {});
                auto const escapes = escape == '\\' ? (ch == '\\' || ch == '"') : ch == escape;
                if (escapes)
                {
                    decoded += ch;
                    continue;
                }
                decoded += escape;
            }
            if (ch == '"')
                break;
            if (ch == '\\' || ch == '%' || ch == '$')
                pending = ch;
            else
                decoded += ch;
        }
        if (pending != char {})
            decoded += pending;
        return decoded;
    }
} // namespace

std::optional<std::filesystem::path> registeredSocketPath(std::string_view commandLine)
{
    auto const at = commandLine.find(SocketOption);
    if (at == std::string_view::npos)
        return std::nullopt;
    auto const quoted = at > 0 && commandLine[at - 1] == '"';
    auto const value = commandLine.substr(at + SocketOption.size());
    auto const path =
        quoted ? unquoteArgument(value) : std::string { value.substr(0, value.find_first_of(" \t")) };
    auto parsed = core::pathFromUtf8(path);
    if (path.empty() || !parsed)
        return std::nullopt;
    return *std::move(parsed);
}

std::vector<std::string> daemonSpawnArgs(std::string_view daemonBinary,
                                         std::filesystem::path const& socketPath,
                                         DaemonSpawnOptions const& spawnOptions,
                                         DaemonLifecycle lifecycle)
{
    auto args = std::vector<std::string> {
        std::string { daemonBinary },
        "daemon",
        std::format("{}{}", SocketOption, core::pathToUtf8(socketPath)),
    };
    // Stated even for Persistent, as --size-policy is: absence is itself a value, and the process list
    // should not need the default to be read.
    args.push_back(std::format("--lifecycle={}", nameOf(lifecycle)));
    if (!spawnOptions.filter.empty())
        args.push_back(std::format("--log={}", spawnOptions.filter));
    // Always logs to a file: a spawned daemon is detached from any console, and `contour daemon logs`
    // reads this file.
    args.push_back(std::format("--log-file={}",
                               spawnOptions.logFile.empty() ? core::pathToUtf8(daemonLogPath(socketPath))
                                                            : spawnOptions.logFile));
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

std::string joinCommandLine(std::span<std::string const> args)
{
    if (args.empty())
        return {};
    return vtpty::buildWindowsCommandLine(args.front(), args.subspan(1));
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
    Require(config.shutdownSignals != nullptr);
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const clock = core::platform::SteadyClock {};

    // Filled as each endpoint is bound below; the funnel reads it when the shutdown comes.
    auto servers = std::vector<ConnectionAcceptor*> {};
    auto funnel = ShutdownFunnel { loop, servers, config.beforeShutdown };

    // Armed before the host, for two reasons. The watch blocks SIGINT/SIGTERM on this thread, and only
    // threads started afterwards inherit that mask: the host starts a pump thread per session of the
    // startup layout in its constructor, and a process-directed SIGTERM reaching a thread that does not
    // block it kills the process by its default action. And it is destroyed after ~SessionHost has
    // joined those threads, so the SIGTERM ~SigwaitWatch sends to release its own thread after a
    // StopDaemon finds none of them left.
    auto const signals = config.shutdownSignals->watch(
        [request = postedShutdown(loop, funnel)] { request(ShutdownCause::Signal); });

    auto host = SessionHost {
        loop,
        makeShellPtyFactory(config.shell, config.escapeSandbox, config.socketPath, config.placement),
        config.settings,
        core::defaultEnvironment(),
        SessionHostConfig { .pumping = SessionPumping::Threaded,
                            .sizePolicy = config.sizePolicy,
                            .startupLayout = config.startupLayout,
                            .identity = thisHostIdentity(),
                            .extensions = config.extensions ? config.extensions() : nullptr,
                            .profiles = config.profiles,
                            .profilePtys =
                                config.profiles
                                    ? makeProfilePtyFactory(config.shell, config.socketPath, config.placement)
                                    : ProfilePtyFactory {},
                            .instance = config.instance }
    };
    // Declared before the drain, as the funnel is: the connection flows the drain unwinds hold its census
    // tickets, and a shutdown request still queued may run during it — a no-op by then, but one that reads
    // the funnel.
    auto runtime = DaemonRuntime { runtimeConfigOf(config), host, clock, postedShutdown(loop, funnel) };
    // An auto-spawned daemon ends when it has nothing left to serve; a user-started one persists.
    // Declared after `host`, so it unsubscribes while the host is still alive, and before the drain, so
    // it outlives it: a sessionClosed in the loop's last turn leaves a post capturing a watcher queued,
    // and the drain runs it. A shutdown request it makes then is harmless: the funnel has shut down
    // already, or every listener died on its own and servers.clear() left it nothing to close.
    auto lifecycleWatchers = LifecycleWatchers {};
    installLifecycleWatchers(lifecycleWatchers, config, host, runtime.census(), loop, funnel);
    // After the host, so its timer is disarmed before the host it hands catalogs to goes.
    auto reloadWatcher = std::optional<ConfigReloadWatcher> {};
    if (config.configReload)
        reloadWatcher.emplace(loop, host, *config.configReload);
    // Declared after the host: every connection flow the servers below spawn subscribes to it, so
    // those flows must end before it is destroyed, not in ~EventLoop after it.
    auto const drain = LoopDrain { loop };

    auto listener = bindDaemonEndpoint(loop, core::pathToUtf8(config.socketPath));
    if (!listener)
        return listener.error();
    auto server = ConnectionAcceptor { loop,
                                       "control",
                                       std::move(*listener),
                                       countedHandler(runtime, tmux::makeControlModeHandler(loop, host)) };

    // The native cells+deltas protocol listens beside the control-mode socket. Its connections count
    // themselves, as only an Attach connection is a client.
    auto const nativePath = core::pathToUtf8(nativeSocketPath(config.socketPath));
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer =
        ConnectionAcceptor { loop,
                             "native",
                             std::move(*nativeListener),
                             makeNativeHandler(loop, host, daemonNativeServices(runtime, config)) };

    // The imsg endpoint serves the REAL tmux client binary
    // (`tmux -S <socket>-tmux -C attach-session`).
    auto const imsgPath = core::pathToUtf8(tmuxSocketPath(config.socketPath));
    auto imsgListener = bindDaemonEndpoint(loop, imsgPath);
    if (!imsgListener)
        return imsgListener.error();
    auto imsgServer = ConnectionAcceptor {
        loop, "imsg", std::move(*imsgListener), countedHandler(runtime, tmux::makeTmuxImsgHandler(loop, host))
    };

    servers = { &server, &nativeServer, &imsgServer };

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
        compatServer.emplace(loop,
                             "tmux-compat",
                             std::move(*compatListener),
                             countedHandler(runtime, tmux::makeTmuxImsgHandler(loop, host)));
        servers.push_back(&*compatServer);
        daemonLog()("tmux-compat socket bound at {}", compatPath);
    }

    // Opt-in: ALSO serve the native protocol over TCP (loopback by default). The
    // transport is protocol-agnostic, so the same native handler serves it — with
    // the preshared token as the authentication the filesystem gate can't provide.
    auto nativeTcpServer = std::optional<ConnectionAcceptor> {};
    if (config.nativeTcp)
    {
        auto tcpListener = core::net::listen(loop, config.nativeTcp->host, config.nativeTcp->port);
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
        auto const boundPort = (*tcpListener)->boundPort();
        nativeTcpServer.emplace(
            loop,
            "native-tcp",
            std::move(*tcpListener),
            makeTlsNativeHandler(
                loop, host, std::move(tls), daemonNativeServices(runtime, config, config.nativeTcp->token)));
        servers.push_back(&*nativeTcpServer);
        daemonLog()("native TCP listener on {}:{}", config.nativeTcp->host, boundPort);
    }

    daemonLog()(
        "serving on {} (native: {}, tmux: {})", core::pathToUtf8(config.socketPath), nativePath, imsgPath);
    loop.blockOn(serveAll(servers));
    // The acceptors are destroyed before the drain below turns the loop again, and a shutdown request
    // still queued may run in it — one that finds the funnel Serving when every listener died on its own.
    // Emptied, the list lets such a request close nothing that is gone.
    servers.clear();

    daemonLog()("shut down");
    return EXIT_SUCCESS;
}

#endif // _WIN32

DaemonPresence awaitDaemon(DaemonSpawner& spawner,
                           std::filesystem::path const& socketPath,
                           std::chrono::milliseconds timeout)
{
    // Poll until the socket accepts connections or the timeout elapses. A freshly started daemon binds
    // within a few ms, so the poll starts at 5 ms and doubles up to a 1s cap, which keeps a long wait
    // from burning connect/close cycles.
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    auto delay = std::optional<core::platform::SteadyDuration> {};
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (spawner.probe(socketPath) == DaemonPresence::Serving)
            return DaemonPresence::Serving;
        delay = nextProbeDelay(delay);
        auto const remaining = deadline - std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::min<core::platform::SteadyDuration>(*delay, remaining));
    }
    return DaemonPresence::Absent;
}

std::unique_ptr<DaemonSpawner> makeNativeDaemonSpawner()
{
    return std::make_unique<NativeDaemonSpawner>();
}

int ensureDaemon(AttachEndpoint const& endpoint,
                 DaemonBinarySource& binary,
                 DaemonSpawner& spawner,
                 vtpty::ProcessPlacement& placement,
                 DaemonSpawnOptions const& spawnOptions,
                 std::chrono::seconds timeout)
{
    // TCP endpoints: no auto-spawn (can't spawn a remote daemon).
    if (std::holds_alternative<TcpEndpoint>(endpoint))
        return EXIT_SUCCESS;

    // Unix socket: probe the native socket beside the control socket.
    auto const& unixEp = std::get<UnixEndpoint>(endpoint);
    if (spawner.probe(unixEp.socketPath) == DaemonPresence::Serving)
        return EXIT_SUCCESS; // daemon is already running

    // Daemon not running — spawn it in the background. A daemon we spawn is ours, so it is started
    // `--lifecycle=exit-when-idle` and ends once nothing needs it any more. The image is asked for only
    // now, and held until the daemon serves: by then the daemon has leased it itself.
    auto const image = binary.imageForSpawn();
    auto const args = daemonSpawnArgs(
        core::pathToUtf8(image.executable), unixEp.socketPath, spawnOptions, DaemonLifecycle::ExitWhenIdle);
    return spawnAndAwaitDaemon(spawner, args, placement, unixEp.socketPath, timeout);
}

int ensureDaemon(AttachEndpoint const& endpoint,
                 DaemonBinarySource& binary,
                 vtpty::ProcessPlacement& placement,
                 DaemonSpawnOptions const& spawnOptions,
                 std::chrono::seconds timeout)
{
    return ensureDaemon(endpoint, binary, *makeNativeDaemonSpawner(), placement, spawnOptions, timeout);
}

int runDaemonDetached(std::vector<std::string> commandLine,
                      std::filesystem::path const& socketPath,
                      vtpty::ProcessPlacement& placement,
                      std::chrono::seconds timeout)
{
    return spawnAndAwaitDaemon(*makeNativeDaemonSpawner(), commandLine, placement, socketPath, timeout);
}

#ifdef _WIN32

int runDaemon(DaemonConfig const& config)
{
    Require(config.shutdownSignals != nullptr);
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const clock = core::platform::SteadyClock {};

    auto servers = std::vector<ConnectionAcceptor*> {};
    auto funnel = ShutdownFunnel { loop, servers, config.beforeShutdown };

    // The console-control handler runs on an arbitrary OS thread; the request only posts, so the
    // shutdown and its log line land on the loop thread like everything else. Armed before the host and
    // destroyed after it, as in the POSIX body above.
    auto const signals = config.shutdownSignals->watch(
        [request = postedShutdown(loop, funnel)] { request(ShutdownCause::Signal); });

    auto host = SessionHost {
        loop,
        makeShellPtyFactory(config.shell, config.escapeSandbox, config.socketPath, config.placement),
        config.settings,
        core::defaultEnvironment(),
        SessionHostConfig { .pumping = SessionPumping::Threaded,
                            .sizePolicy = config.sizePolicy,
                            .startupLayout = config.startupLayout,
                            .identity = thisHostIdentity(),
                            .extensions = config.extensions ? config.extensions() : nullptr,
                            .profiles = config.profiles,
                            .profilePtys =
                                config.profiles
                                    ? makeProfilePtyFactory(config.shell, config.socketPath, config.placement)
                                    : ProfilePtyFactory {},
                            .instance = config.instance }
    };
    // See the POSIX body above for why the runtime is declared here, before the drain.
    auto runtime = DaemonRuntime { runtimeConfigOf(config), host, clock, postedShutdown(loop, funnel) };
    // See the POSIX body above for why the lifecycle watchers are declared here, before the drain.
    auto lifecycleWatchers = LifecycleWatchers {};
    installLifecycleWatchers(lifecycleWatchers, config, host, runtime.census(), loop, funnel);
    // After the host, so its timer is disarmed before the host it hands catalogs to goes.
    auto reloadWatcher = std::optional<ConfigReloadWatcher> {};
    if (config.configReload)
        reloadWatcher.emplace(loop, host, *config.configReload);
    // Declared after the host: every connection flow the servers below spawn subscribes to it, so
    // those flows must end before it is destroyed, not in ~EventLoop after it.
    auto const drain = LoopDrain { loop };

    auto listener = bindDaemonEndpoint(loop, core::pathToUtf8(config.socketPath));
    if (!listener)
        return listener.error();
    auto server = ConnectionAcceptor { loop,
                                       "control",
                                       std::move(*listener),
                                       countedHandler(runtime, tmux::makeControlModeHandler(loop, host)) };

    auto const nativePath = core::pathToUtf8(nativeSocketPath(config.socketPath));
    auto nativeListener = bindDaemonEndpoint(loop, nativePath);
    if (!nativeListener)
        return nativeListener.error();
    auto nativeServer =
        ConnectionAcceptor { loop,
                             "native",
                             std::move(*nativeListener),
                             makeNativeHandler(loop, host, daemonNativeServices(runtime, config)) };
    // No imsg endpoint on Windows: SCM_RIGHTS fd passing does not exist here.

    servers = { &server, &nativeServer };

    // Opt-in native TCP listener (loopback by default), same as the POSIX path.
    auto nativeTcpServer = std::optional<ConnectionAcceptor> {};
    if (config.nativeTcp)
    {
        auto tcpListener = core::net::listen(loop, config.nativeTcp->host, config.nativeTcp->port);
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
        auto const boundPort = (*tcpListener)->boundPort();
        nativeTcpServer.emplace(
            loop,
            "native-tcp",
            std::move(*tcpListener),
            makeTlsNativeHandler(
                loop, host, std::move(tls), daemonNativeServices(runtime, config, config.nativeTcp->token)));
        servers.push_back(&*nativeTcpServer);
        daemonLog()("native TCP listener on {}:{}", config.nativeTcp->host, boundPort);
    }

    daemonLog()("serving on {} (native: {})", core::pathToUtf8(config.socketPath), nativePath);
    loop.blockOn(serveAll(servers));
    // The acceptors are destroyed before the drain below turns the loop again, and a shutdown request
    // still queued may run in it — one that finds the funnel Serving when every listener died on its own.
    // Emptied, the list lets such a request close nothing that is gone.
    servers.clear();

    daemonLog()("shut down");
    return EXIT_SUCCESS;
}

#endif // _WIN32

} // namespace vthost
