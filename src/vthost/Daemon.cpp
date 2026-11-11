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
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/ProgramLookup.hpp>
#include <vtpty/WindowsCommandLine.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/WideString.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/SystemInfo.hpp>
#include <core/platform/Types.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/ConnectionRegistry.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/DaemonRuntime.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/DaemonUpgradeHooks.hpp>
#include <vthost/FilePrivacy.hpp>
#include <vthost/HostedShell.hpp>
#include <vthost/IdleExitWatcher.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vthost/ListeningSocket.hpp>
#include <vthost/Logging.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshotStore.hpp>
#include <vthost/ShutdownFunnel.hpp>
#include <vthost/ShutdownSignals.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/WorkspacePersistence.hpp>
#include <vthost/control/ControlService.hpp>
#include <vthost/handover/LiveUpgrade.hpp>
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

    /// The TLS a native TCP listener serves with.
    struct NativeTcpTls
    {
        std::shared_ptr<core::net::ITlsContext> context; ///< What encrypts each connection.
        /// The certificate and key the daemon made up for it, which a live upgrade carries so that the new
        /// image presents the very same one; nullopt for a configured certificate, which it reads again.
        std::optional<handover::TlsIdentity> generated;
    };

    /// Builds the TLS server context for the native TCP listener: the configured PEM cert + key; else the
    /// self-signed identity a live upgrade carried over, so that a client that trusted it on first use goes
    /// on trusting it; else an ephemeral self-signed one, made up now (the TOFU default). The TCP transport
    /// is always encrypted.
    /// @param config The listener's configuration.
    /// @param carried The identity the previous image of this daemon presented on it; nullopt for none.
    /// @return The TLS, or nullopt after logging why it could not be built.
    [[nodiscard]] std::optional<NativeTcpTls> makeNativeTcpTls(NativeTcpListenerConfig const& config,
                                                               std::optional<handover::TlsIdentity> carried)
    {
        if (config.tlsCertPath.empty() && config.tlsKeyPath.empty())
        {
            if (carried)
            {
                if (auto context = core::net::makeTlsServerContext(carried->certPem, carried->keyPem))
                {
                    daemonLog()(
                        "native TCP keeps the self-signed certificate it presented before the upgrade");
                    return NativeTcpTls { .context = *std::move(context), .generated = std::move(carried) };
                }
                else
                    errorLog()("cannot present the carried TLS certificate again ({}); clients that trusted "
                               "it will be asked to trust a new one",
                               context.error());
            }
            auto material = core::net::generateSelfSignedCertificate({ .commonName = "contour-daemon" });
            auto context = material.and_then([](core::net::CertKeyPem const& pem) {
                return core::net::makeTlsServerContext(pem.certPem, pem.keyPem);
            });
            if (!context)
            {
                errorLog()("cannot build an ephemeral TLS certificate: {}", context.error());
                return std::nullopt;
            }
            daemonLog()("native TCP is using an ephemeral self-signed certificate (TOFU)");
            return NativeTcpTls {
                .context = *std::move(context),
                .generated = handover::TlsIdentity { .certPem = std::move(material->certPem),
                                                     .keyPem = std::move(material->keyPem) },
            };
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
            return std::nullopt;
        }
        auto context = core::net::makeTlsServerContext(*cert, *key);
        if (!context)
        {
            errorLog()("cannot build the TLS server context: {}", context.error());
            return std::nullopt;
        }
        return NativeTcpTls { .context = *std::move(context), .generated = std::nullopt };
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

    /// @param endpoint The endpoint's name ("control", "native", ...).
    /// @return What the first client is told of an endpoint a daemon serves on without after an upgrade.
    [[nodiscard]] std::string unservedEndpointNotice(std::string_view endpoint)
    {
        return std::format(
            "Contour could not serve its {} endpoint after the upgrade, and runs on without it; "
            "the daemon's log says why",
            endpoint);
    }

    /// Waits until the daemon is asked to end: what a daemon that took over hosted programs but serves no
    /// endpoint at all does, rather than ending at once and hanging them up. The shutdown cancels every flow
    /// of the loop (ShutdownFunnel::request()), this wait among them, which is how it ends.
    /// @param loop The daemon's loop (not owned; outlives the wait).
    core::async::Task<void> awaitShutdown(core::net::EventLoop* loop)
    {
        try
        {
            while (true)
                co_await loop->delay(std::chrono::hours { 1 });
        }
        catch (core::async::OperationCancelled const&)
        {
            co_return;
        }
    }

    /// Closes @p listener, inherited and not served: its socket, which on Windows only closesocket() closes.
    void closeInherited(handover::InheritedListener const& listener) noexcept
    {
        if (listener.handle.isNone())
            return;
#ifdef _WIN32
        ::closesocket(reinterpret_cast<SOCKET>(listener.handle.native()));
#else
        core::platform::platformClose(listener.handle.native());
#endif
    }

    /// The listening sockets a handover image hands this process, each taken at most once, by name; what is
    /// not taken is closed (closeUnclaimed(), or at the latest with this), so that no endpoint the new
    /// configuration dropped stays bound unserved.
    class InheritedListeners
    {
      public:
        /// @param listeners The listeners the handover image names, taken from it; none for a daemon that
        ///        starts afresh.
        explicit InheritedListeners(std::vector<handover::InheritedListener> listeners):
            _pending { std::move(listeners) }
        {
        }
        InheritedListeners(InheritedListeners const&) = delete;
        InheritedListeners& operator=(InheritedListeners const&) = delete;
        InheritedListeners(InheritedListeners&&) = delete;
        InheritedListeners& operator=(InheritedListeners&&) = delete;
        ~InheritedListeners() { closeUnclaimed(); }

        /// Adopts the listener inherited as @p name.
        /// @return It; or nullopt when none was, or when it could not be adopted, which closes it.
        [[nodiscard]] std::optional<std::unique_ptr<core::net::IListener>> take(core::net::EventLoop& loop,
                                                                                std::string_view name)
        {
            auto const found = std::ranges::find(_pending, name, &handover::InheritedListener::name);
            if (found == _pending.end())
                return std::nullopt;
            // Moved out before it is erased: it is this call's to adopt or to close from here on.
            auto const listener = std::move(*found);
            _pending.erase(found);
            auto adopted = core::net::adoptListener(loop, listener.handle.native());
            if (!adopted)
            {
                errorLog()("cannot serve the inherited {} listener: {}", name, adopted.error().toString());
                closeInherited(listener);
                return std::nullopt;
            }
            daemonLog()("serving the inherited {} listener", name);
            return std::move(*adopted);
        }

        /// Takes the TLS identity the listener inherited as @p name presented, so that it presents the very
        /// same one again; the listener itself stays to be taken.
        /// @return It; or nullopt for none, or when it was taken already.
        [[nodiscard]] std::optional<handover::TlsIdentity> takeTlsIdentity(std::string_view name)
        {
            auto const found = std::ranges::find(_pending, name, &handover::InheritedListener::name);
            if (found == _pending.end())
                return std::nullopt;
            return std::exchange(found->tlsIdentity, std::nullopt);
        }

        /// Closes every listener not taken.
        void closeUnclaimed()
        {
            for (auto const& listener: std::exchange(_pending, {}))
            {
                daemonLog()("closing the inherited {} listener, which this configuration does not serve",
                            listener.name);
                closeInherited(listener);
            }
        }

      private:
        std::vector<handover::InheritedListener> _pending;
    };

    /// Serves @p path on the listener inherited as @p name, else binds it.
    /// @return The listener, or the exit code a bind failure ends the daemon with.
    [[nodiscard]] std::expected<std::unique_ptr<core::net::IListener>, int> adoptOrBindEndpoint(
        core::net::EventLoop& loop,
        InheritedListeners& inherited,
        std::string_view name,
        std::string const& path)
    {
        if (auto adopted = inherited.take(loop, name))
            return std::move(*adopted);
        return bindDaemonEndpoint(loop, path);
    }

    /// Which run of the daemon this is, and how many live upgrades it has seen: what an upgrade carries on.
    struct DaemonLineage
    {
        uint64_t instance = 0;          ///< The run, as every ServerHello states it.
        uint64_t upgradeGeneration = 0; ///< The live upgrades it completed (proto::DaemonUpgradeRecord).
    };

    /// @param config The daemon's configuration.
    /// @param image The handover image it starts from; nullopt for a daemon started afresh.
    /// @return The run it serves as: the image's, one upgrade further, or a fresh run of @p config's.
    [[nodiscard]] DaemonLineage lineageOf(DaemonConfig const& config,
                                          std::optional<handover::HandoverImage> const& image)
    {
        if (!image)
            return DaemonLineage { .instance = config.instance, .upgradeGeneration = 0 };
        return DaemonLineage { .instance = image->daemonInstance,
                               .upgradeGeneration = image->upgradeGeneration + 1 };
    }

    /// @param config The daemon's configuration.
    /// @param services What it upgrades itself with.
    /// @param lineage The run of the daemon it serves as, which the upgrade carries on.
    /// @param tlsIdentities The TLS identities it made up for its listeners, by listener name.
    /// @return How its live upgrade runs: as this release, of this run, repeating this command line.
    [[nodiscard]] handover::LiveUpgradeConfig liveUpgradeConfigOf(
        DaemonConfig const& config,
        LiveUpgradeServices const& services,
        DaemonLineage lineage,
        std::map<std::string, handover::TlsIdentity, std::less<>> tlsIdentities)
    {
        return handover::LiveUpgradeConfig { .release = config.releaseVersion,
                                             .daemonInstance = lineage.instance,
                                             .upgradeGeneration = lineage.upgradeGeneration,
                                             .argv = services.argv,
                                             .installedExecutable = services.installedExecutable,
                                             .sandbox = services.sandbox,
                                             .configFile = services.configFile,
                                             .profile = services.profile,
                                             .runningConfig = services.runningConfig,
                                             .tlsIdentities = std::move(tlsIdentities) };
    }

    /// @param config The daemon's configuration.
    /// @param hooks The daemon's upgrade hooks (not owned; outlives the port).
    /// @param upgrade Where the daemon's LiveUpgrade is built once its listeners are (not owned; outlives the
    ///        port, and holds one before any connection can ask for it).
    /// @return The port its runtime upgrades it through; nullopt for a daemon given nothing to upgrade with.
    [[nodiscard]] std::optional<DaemonUpgradePort> upgradePortOf(
        DaemonConfig const& config, DaemonUpgradeHooks& hooks, std::optional<handover::LiveUpgrade>& upgrade)
    {
        if (!config.liveUpgrade)
            return std::nullopt;
        return DaemonUpgradePort {
            .hooks = hooks,
            .run = [&upgrade](handover::UpgradeTarget target) { return upgrade->run(std::move(target)); },
            .state = [&upgrade] { return upgrade ? upgrade->state() : handover::UpgradeState::Idle; },
        };
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
    /// @param continuity Whether the daemon took over from a handover, or started afresh.
    /// @param host The host whose sessions are observed.
    /// @param census The daemon's client count.
    /// @param loop The loop the deferred shutdown is posted onto.
    /// @param funnel The daemon's shutdown (not owned; must outlive @p watchers).
    void installLifecycleWatchers(LifecycleWatchers& watchers,
                                  DaemonConfig const& config,
                                  StartContinuity continuity,
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
                // The watcher sees a last session close, which a daemon that took over from a handover may
                // have seen already: the previous image's own shutdown was lost to the exec, or no session
                // could be adopted. Looked at once, as the start's own last close.
                if (continuity == StartContinuity::HandedOver && host.sessionCount() == 0)
                {
                    daemonLog()("taken over from a handover with no session left; terminating");
                    loop.post([&funnel] { funnel.request(ShutdownCause::LastSessionClosed); });
                }
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

    /// The workspace persistence's one-line diagnostics, into the daemon's log. The writer thread calls it
    /// too, which the log allows: its sinks serialize their writes.
    /// @param line The diagnostic.
    void logPersistence(std::string_view line)
    {
        daemonLog()("{}", line);
    }

    /// @param config How the daemon keeps its workspace.
    /// @return The snapshot file on the native file system; nullopt while persistence is off.
    [[nodiscard]] std::optional<SessionSnapshotStore> snapshotStoreOf(
        WorkspacePersistenceConfig const& config)
    {
        if (config.snapshotPath.empty())
            return std::nullopt;
        daemonLog()("the workspaces are kept in {}", core::pathToUtf8(config.snapshotPath));
        return SessionSnapshotStore { core::platform::NativeFileSystem::instance(),
                                      NativeFilePrivacy::instance(),
                                      config.snapshotPath };
    }

    /// @param store The snapshot file, while persistence is on.
    /// @param config How the daemon keeps its workspace.
    /// @param wall The daemon's wall clock, which dates the restore divider.
    /// @return What the host restores at its start; nullopt for nothing.
    [[nodiscard]] std::optional<WorkspaceRestore> restoreOf(std::optional<SessionSnapshotStore> const& store,
                                                            WorkspacePersistenceConfig const& config,
                                                            core::platform::WallClockRef wall)
    {
        if (!store)
            return std::nullopt;
        return prepareRestore(*store, config, logPersistence, wall);
    }

    class BoundPersistence;

    /// What a daemon does before its listeners close, in order: the workspace's last snapshot, the caller's
    /// beforeShutdown, then the word that lets a console-control handler holding the process's end return
    /// (ShutdownSignals::watch's lastWrite). Built before the funnel that runs it and the signal watch that
    /// waits on it; the persistence it finalizes exists only once the host does, so a BoundPersistence binds
    /// itself in for its own lifetime.
    class ShutdownSequence
    {
      public:
        /// @param callerHook DaemonConfig::beforeShutdown (not owned; outlives this); may be empty.
        explicit ShutdownSequence(std::function<void(ShutdownCause)> const& callerHook):
            _callerHook { callerHook }, _lastWrite { _lastWritten.get_future().share() }
        {
        }

        /// Runs the sequence. Loop thread; once, as the funnel runs its hook.
        /// @param cause What ends the daemon.
        void run(ShutdownCause cause)
        {
            if (_persistence != nullptr)
                _persistence->finalize(cause);
            if (_callerHook)
                _callerHook(cause);
            _lastWritten.set_value();
        }

        /// @return Ready once run() is done.
        [[nodiscard]] std::shared_future<void> lastWrite() const { return _lastWrite; }

      private:
        friend class BoundPersistence;

        std::function<void(ShutdownCause)> const& _callerHook;
        DaemonPersistence* _persistence = nullptr; ///< Set by the BoundPersistence alive, if any.
        std::promise<void> _lastWritten;
        std::shared_future<void> _lastWrite;
    };

    /// A daemon's DaemonPersistence, bound into its ShutdownSequence for as long as it lives.
    class BoundPersistence
    {
      public:
        /// Starts keeping @p host's workspace; see DaemonPersistence for the parameters.
        /// @param sequence The shutdown sequence that finalizes it (not owned; outlives this).
        BoundPersistence(ShutdownSequence& sequence,
                         core::net::EventLoop& loop,
                         SessionHost& host,
                         SessionSnapshotStore const& store,
                         std::optional<RestoreSource> restoredFrom,
                         WorkspacePersistenceConfig const& config,
                         core::platform::IWallClock const& wallClock):
            _persistence(loop,
                         host,
                         store,
                         restoredFrom,
                         config,
                         wallClock,
                         WriterThreading::Background,
                         logPersistence),
            _sequence { sequence }
        {
            _sequence._persistence = &_persistence;
        }

        BoundPersistence(BoundPersistence const&) = delete;
        BoundPersistence& operator=(BoundPersistence const&) = delete;
        BoundPersistence(BoundPersistence&&) = delete;
        BoundPersistence& operator=(BoundPersistence&&) = delete;

        ~BoundPersistence() { _sequence._persistence = nullptr; }

        /// Writes the workspace now and blocks until it is on disk, sealing nothing (@see
        /// DaemonPersistence::writeNow). Loop thread only.
        void writeNow() { _persistence.writeNow(); }

      private:
        DaemonPersistence _persistence;
        ShutdownSequence& _sequence;
    };

    /// @param config The daemon's configuration.
    /// @param upgradeGeneration How many live upgrades this run of the daemon has completed.
    /// @return What its DaemonRuntime states about it.
    [[nodiscard]] DaemonRuntimeConfig runtimeConfigOf(DaemonConfig const& config, uint64_t upgradeGeneration)
    {
        return DaemonRuntimeConfig { .releaseVersion = config.releaseVersion,
                                     .processId = currentProcessId(),
                                     .socketPath = config.socketPath,
                                     .executablePath = config.executablePath,
                                     .logFile = config.logFile,
                                     .lifecycle = config.lifecycle,
                                     .upgradeGeneration = upgradeGeneration,
                                     .relocationRequest = config.relocationRequest };
    }

    /// What every native endpoint of a daemon serves its connections with: @p runtime behind them, and a
    /// handshake that offers DaemonControl, because @p runtime answers it. Only a daemon offers it; an
    /// embedded host's default handshake keeps not claiming what its null daemon control refuses.
    /// @param runtime The daemon (not owned; outlives every connection).
    /// @param control The daemon's control API (not owned; outlives every connection).
    /// @param config The daemon's configuration: with profiles, the handshake offers ProfileByName too.
    /// @param acceptedTokens The tokens a client may present, with their grants; empty for the unix endpoint,
    ///        whose socket permissions are the gate.
    /// @return The services.
    [[nodiscard]] NativeSessionServices daemonNativeServices(DaemonRuntime& runtime,
                                                             vtcontrol::ControlApi& control,
                                                             DaemonConfig const& config,
                                                             std::vector<AcceptedToken> acceptedTokens = {})
    {
        auto capabilities = proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions,
                                                       proto::Capability::DaemonControl,
                                                       proto::Capability::InitialClientArea,
                                                       proto::Capability::AttachIntent,
                                                       proto::Capability::TabEdits,
                                                       proto::Capability::Heartbeat,
                                                       proto::Capability::ControlApi,
                                                       proto::Capability::ProgramStatus,
                                                       proto::Capability::ProgramStatusTiming,
                                                       proto::Capability::ControlPermissions,
                                                       proto::Capability::InputEcho,
                                                       proto::Capability::TabMerge,
                                                       proto::Capability::MoveNode,
                                                       proto::Capability::LayoutBatch,
                                                       proto::Capability::TabEditAcks,
                                                       proto::Capability::ColorPreference,
                                                       proto::Capability::CellPixelSize,
                                                       proto::Capability::ClipboardTargets,
                                                       proto::Capability::SoftFonts,
                                                       proto::Capability::WorkspaceFollow,
                                                       proto::Capability::WorkspaceSwitch,
                                                       proto::Capability::WorkspaceShown });
        // Only a daemon with profiles resolves a name; one without keeps honouring a client's settings.
        if (config.profiles)
            capabilities.idents.push_back(std::to_underlying(proto::Capability::ProfileByName));
        // Only a daemon that keeps its workspace brings it back after a restart.
        if (!config.persistence.snapshotPath.empty())
            capabilities.idents.push_back(std::to_underlying(proto::Capability::WorkspaceRestore));
        // Only a daemon wired to upgrade itself in place announces it, and takes the request.
        if (runtime.canUpgrade())
            capabilities.idents.push_back(std::to_underlying(proto::Capability::LiveUpgrade));
        return NativeSessionServices {
            .handshake = { .acceptedTokens = std::move(acceptedTokens),
                           .capabilities = std::move(capabilities) },
            .daemon = runtime,
            .control = control,
        };
    }

    /// @param config The daemon's configuration.
    /// @param clock What the service reads uptimes from (not owned; outlives the service).
    /// @return How the daemon's control service is set up: a reply carries at most a quarter of a native
    ///         connection's send-queue bound of pane text, so that it always fits beside a backlog.
    [[nodiscard]] control::ControlServiceConfig controlServiceConfig(DaemonConfig const& config,
                                                                     core::platform::IWallClock const& clock,
                                                                     core::net::EventLoop& loop,
                                                                     DaemonControl& daemon)
    {
        return control::ControlServiceConfig {
            .version = config.releaseVersion,
            .socketPath = config.socketPath,
            .clock = clock,
            .maxReplyBytes = NativeSession::DefaultWriteQueueBytes / 4,
            // The same flow the native UpgradeDaemon runs: one upgrade path, whoever asks.
            .upgrade =
                [&loop,
                 &daemon](handover::UpgradeTarget target, UpgradeStarted onStarted, UpgradeFailed onFailed) {
                    loop.spawn(
                        serveUpgrade(&daemon, std::move(target), std::move(onStarted), std::move(onFailed)));
                },
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
    /// @param environment What the daemon runs under ("NAME=VALUE"); std::nullopt for this process's own.
    /// @param placement Places the daemon before it runs, as a session's shell is placed. POSIX only.
    /// @return Nothing, or why the process could not be started.
    [[nodiscard]] std::expected<void, std::string> spawnDetachedDaemon(
        std::span<std::string const> args,
        std::optional<std::vector<std::string>> const& environment,
        [[maybe_unused]] vtpty::ProcessPlacement& placement)
    {
        if (args.empty())
            return std::unexpected("no program to start");
#ifndef _WIN32
        // Placed, as a session's shell is: the daemon holds the scrollback of every session it hosts,
        // and an out-of-memory kill elsewhere in the client's unit, or in the tab it was typed into,
        // must not end it.
        return vtpty::startDetached(daemonDetachedCommand(args,
                                                          environment,
                                                          core::defaultEnvironment(),
                                                          core::platform::NativeFileSystem::instance()),
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
        // A given environment goes as a block of its own; none inherits this process's.
        auto block = std::optional<std::wstring> {};
        if (environment)
        {
            block = vtpty::windowsEnvironmentBlock(*environment);
            if (!block)
                return std::unexpected("the environment is not valid UTF-8");
        }
        auto creationFlags = DWORD { DETACHED_PROCESS | CREATE_NO_WINDOW };
        if (block)
            creationFlags |= CREATE_UNICODE_ENVIRONMENT;
        if (!::CreateProcessW(nullptr,
                              commandLine->data(),
                              nullptr,
                              nullptr,
                              FALSE,
                              creationFlags,
                              block ? block->data() : nullptr,
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
        /// @param environment What every daemon it spawns runs under; std::nullopt for this process's own.
        explicit NativeDaemonSpawner(std::optional<std::vector<std::string>> environment):
            _environment { std::move(environment) },
            _source { core::net::makeDefaultBackend() },
            _loop { *_source }
        {
        }

        [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                             vtpty::ProcessPlacement& placement) override
        {
            return spawnDetachedDaemon(argv, _environment, placement);
        }

        [[nodiscard]] DaemonPresence probe(std::filesystem::path const& socketPath) override
        {
            return daemonAccepts(_loop, core::pathToUtf8(nativeSocketPath(socketPath)))
                       ? DaemonPresence::Serving
                       : DaemonPresence::Absent;
        }

      private:
        std::optional<std::vector<std::string>> _environment;
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

std::vector<AcceptedToken> acceptedTokensOf(NativeTcpListenerConfig const& listener)
{
    auto tokens = std::vector<AcceptedToken> {};
    if (!listener.token.empty())
        tokens.push_back(
            AcceptedToken { .token = listener.token, .grants = vtcontrol::ControlGrants::all() });
    tokens.insert(tokens.end(), listener.accessTokens.begin(), listener.accessTokens.end());
    return tokens;
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

int runDaemon(DaemonConfig const& config, std::optional<handover::HandoverImage> image)
{
    Require(config.shutdownSignals != nullptr);
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const clock = core::platform::SteadyClock {};
    auto const wallClock = core::platform::SystemWallClock {};

    // Filled as each endpoint is bound below; the funnel reads it when the shutdown comes.
    auto servers = std::vector<ConnectionAcceptor*> {};
    auto sequence = ShutdownSequence { config.beforeShutdown };
    auto funnel = ShutdownFunnel { loop, servers, [&sequence](ShutdownCause cause) { sequence.run(cause); } };

    // Armed before the host, for two reasons. The watch blocks SIGINT/SIGTERM on this thread, and only
    // threads started afterwards inherit that mask: the host starts a pump thread per session of the
    // startup layout in its constructor (and the persistence its writer thread), and a process-directed
    // SIGTERM reaching a thread that does not block it kills the process by its default action. And it is
    // destroyed after ~SessionHost has joined those threads, so the SIGTERM ~SigwaitWatch sends to release
    // its own thread after a StopDaemon finds none of them left.
    auto const signals = config.shutdownSignals->watch(
        [request = postedShutdown(loop, funnel)] { request(ShutdownCause::Signal); }, sequence.lastWrite());

    // What a previous image of this daemon handed over is consumed as it is taken over, part by part, and
    // kept nowhere beside: its listening sockets first, served on rather than bound again, which a client
    // reconnecting finds without a gap; the rest by the host.
    auto const continuity = image ? StartContinuity::HandedOver : StartContinuity::Fresh;
    auto inherited = InheritedListeners { image ? std::exchange(image->listeners, {})
                                                : std::vector<handover::InheritedListener> {} };
    // What the daemon could not start as configured, for the first client to be told. Only a daemon that took
    // over from a handover ever serves on so: ending would hang up every program it adopted.
    auto degradations = config.degradations;

    // The control socket is bound before the snapshot is touched: a second daemon on the same label fails
    // here, rather than after moving aside the file the serving one keeps writing.
    auto listener = adoptOrBindEndpoint(loop, inherited, "control", core::pathToUtf8(config.socketPath));
    if (!listener && continuity == StartContinuity::Fresh)
        return listener.error();
    if (!listener)
        degradations.push_back(unservedEndpointNotice("control"));

    // Read before the host, which realizes it in its constructor; the store outlives the persistence below.
    auto const store = snapshotStoreOf(config.persistence);
    // A handover carries the workspace itself: the snapshot stays where it is, for a restart after this run.
    auto restore = image ? std::nullopt : restoreOf(store, config.persistence, wallClock);
    auto const restoredFrom =
        restore.transform([](WorkspaceRestore const& restored) { return restored.source; });
    // The same run of the daemon goes on across a live upgrade, one upgrade further.
    auto const lineage = lineageOf(config, image);

    auto host = SessionHost {
        loop,
        makeShellPtyFactory(config.shell, config.escapeSandbox, config.socketPath, config.placement),
        config.settings,
        core::defaultEnvironment(),
        SessionHostConfig { .pumping = SessionPumping::Threaded,
                            .sizePolicy = config.sizePolicy,
                            .defaultCellPixelSize = config.defaultCellPixelSize,
                            .startupLayout = config.startupLayout,
                            .identity = thisHostIdentity(),
                            .extensions = config.extensions ? config.extensions() : nullptr,
                            .profiles = config.profiles,
                            .profilePtys =
                                config.profiles
                                    ? makeProfilePtyFactory(config.shell, config.socketPath, config.placement)
                                    : ProfilePtyFactory {},
                            .instance = lineage.instance,
                            .restore = std::move(restore),
                            .handover = std::exchange(image, std::nullopt) }
    };
    if (auto const outcome = host.handoverOutcome(); !outcome)
        errorLog()("the handover was taken over in part (error {})", std::to_underlying(outcome.error()));
    // Every endpoint below that cannot be served is skipped, and said so; the host keeps what is to be told.
    auto const unserved = [&host](std::string_view endpoint) {
        host.announceDegradation(unservedEndpointNotice(endpoint));
    };
    for (auto& notice: std::exchange(degradations, {}))
        host.announceDegradation(std::move(notice));
    // After the host, so its timer is disarmed before the host it hands catalogs to goes.
    auto reloadWatcher = std::optional<ConfigReloadWatcher> {};
    if (config.configReload)
        reloadWatcher.emplace(loop, host, *config.configReload);
    // After the host, which it watches and must go first, and before the drain, which may still run the
    // shutdown it finalizes in; before any listener serves, so no client changes what it has not seen.
    // Before the upgrade hooks, which write the workspace through it.
    auto persistence = std::optional<BoundPersistence> {};
    if (store)
        persistence.emplace(sequence, loop, host, *store, restoredFrom, config.persistence, wallClock);
    // The live upgrade's side of the daemon: the connections that are not the native protocol's (tmux control
    // mode, imsg, tmux-compat), which it closes outright, and the hooks that tell, write and close. Declared
    // before the runtime that holds the hooks and the drain that unwinds the tracked connections.
    auto const connections = ConnectionRegistry {};
    auto upgradeHooks =
        DaemonUpgradeHooks { connections,
                             persistence
                                 ? std::function<void()> { [&persistence] { persistence->writeNow(); } }
                                 : std::function<void()> {} };
    // Built only once every listener below is, which it holds while it captures: the runtime it is reached
    // through serves the connections those listeners accept, so the three wire in a cycle that only this
    // late emplacement breaks. Nothing reaches it before then, as nothing is accepted before then. Declared
    // before the drain, which may still unwind an upgrade under way.
    auto liveUpgrade = std::optional<handover::LiveUpgrade> {};
    // Declared before the drain, as the funnel is: the connection flows the drain unwinds hold its census
    // tickets, and a shutdown request still queued may run during it — a no-op by then, but one that reads
    // the funnel.
    auto runtime = DaemonRuntime { runtimeConfigOf(config, lineage.upgradeGeneration),
                                   host,
                                   clock,
                                   postedShutdown(loop, funnel),
                                   upgradePortOf(config, upgradeHooks, liveUpgrade) };
    // Declared after the host it serves and before the drain, as the runtime is: the connection flows the
    // drain unwinds drop their subscriptions and waits in it, and its own subscriptions to the host end
    // before the host goes.
    auto controlService =
        control::ControlService { host, controlServiceConfig(config, wallClock, loop, runtime) };
    // An auto-spawned daemon ends when it has nothing left to serve; a user-started one persists.
    // Declared after `host`, so it unsubscribes while the host is still alive, and before the drain, so
    // it outlives it: a sessionClosed in the loop's last turn leaves a post capturing a watcher queued,
    // and the drain runs it. A shutdown request it makes then is harmless: the funnel has shut down
    // already, or every listener died on its own and servers.clear() left it nothing to close.
    auto lifecycleWatchers = LifecycleWatchers {};
    installLifecycleWatchers(lifecycleWatchers, config, continuity, host, runtime.census(), loop, funnel);
    // Declared after the host: every connection flow the servers below spawn subscribes to it, so
    // those flows must end before it is destroyed, not in ~EventLoop after it.
    auto const drain = LoopDrain { loop };

    // Serves the endpoint @p name at @p path, adopted or bound, with the handler @p makeHandler makes. One
    // that cannot be served fails a fresh daemon (the exit code returned), and is skipped by a handed-over
    // one, which says so.
    auto const serveEndpoint = [&](std::optional<ConnectionAcceptor>& acceptor,
                                   std::string_view name,
                                   std::string const& path,
                                   auto const& makeHandler) -> std::expected<void, int> {
        auto bound = adoptOrBindEndpoint(loop, inherited, name, path);
        if (!bound)
        {
            if (continuity == StartContinuity::Fresh)
                return std::unexpected { bound.error() };
            unserved(name);
            return {};
        }
        acceptor.emplace(loop, std::string { name }, std::move(*bound), makeHandler());
        servers.push_back(&*acceptor);
        return {};
    };

    auto server = std::optional<ConnectionAcceptor> {};
    if (listener)
    {
        server.emplace(
            loop,
            "control",
            std::move(*listener),
            countedHandler(runtime, connections.tracked(tmux::makeControlModeHandler(loop, host))));
        servers.push_back(&*server);
    }

    // The native cells+deltas protocol listens beside the control-mode socket. Its connections count
    // themselves, as only an Attach connection is a client.
    auto const nativePath = core::pathToUtf8(nativeSocketPath(config.socketPath));
    auto nativeServer = std::optional<ConnectionAcceptor> {};
    if (auto const served = serveEndpoint(
            nativeServer,
            "native",
            nativePath,
            [&] {
                return makeNativeHandler(loop, host, daemonNativeServices(runtime, controlService, config));
            });
        !served)
        return served.error();

    // The imsg endpoint serves the REAL tmux client binary
    // (`tmux -S <socket>-tmux -C attach-session`). Not on Windows: SCM_RIGHTS descriptor passing, which it
    // needs, does not exist there.
    auto imsgServer = std::optional<ConnectionAcceptor> {};
    auto compatServer = std::optional<ConnectionAcceptor> {};
#ifndef _WIN32
    auto const makeImsgHandler = [&] {
        return countedHandler(runtime, connections.tracked(tmux::makeTmuxImsgHandler(loop, host)));
    };
    auto const imsgPath = core::pathToUtf8(tmuxSocketPath(config.socketPath));
    if (auto const served = serveEndpoint(imsgServer, "imsg", imsgPath, makeImsgHandler); !served)
        return served.error();

    // Opt-in: ALSO bind tmux's own discovery path, so a plain
    // `tmux -L <label> -C attach-session` finds this daemon. Opt-in only —
    // with the daemon down, a `new-session` on that path silently forks a
    // REAL tmux server onto it.
    if (config.tmuxCompatLabel)
    {
        auto const compatPath = std::format("/tmp/tmux-{}/{}", ::getuid(), *config.tmuxCompatLabel);
        if (auto const served = serveEndpoint(compatServer, "tmux-compat", compatPath, makeImsgHandler);
            !served)
            return served.error();
        if (compatServer)
            daemonLog()("tmux-compat socket bound at {}", compatPath);
    }
#endif

    // Opt-in: ALSO serve the native protocol over TCP (loopback by default). The
    // transport is protocol-agnostic, so the same native handler serves it — with
    // the preshared token as the authentication the filesystem gate can't provide.
    auto nativeTcpServer = std::optional<ConnectionAcceptor> {};
    // The identities the daemon made up for its listeners, which cross an upgrade with them.
    auto tlsIdentities = std::map<std::string, handover::TlsIdentity, std::less<>> {};
    if (config.nativeTcp)
    {
        // Built before the listener is taken, so that a listener it cannot encrypt is closed, unserved.
        auto tls = makeNativeTcpTls(*config.nativeTcp, inherited.takeTlsIdentity("native-tcp"));
        auto listened = std::optional<std::unique_ptr<core::net::IListener>> {};
        if (tls)
        {
            if (auto adopted = inherited.take(loop, "native-tcp"))
                listened = std::move(*adopted);
            else if (auto bound = core::net::listen(loop, config.nativeTcp->host, config.nativeTcp->port))
                listened = std::move(*bound);
            else
                errorLog()("cannot listen on {}:{}: {}",
                           config.nativeTcp->host,
                           config.nativeTcp->port,
                           bound.error().toString());
        }
        if (!listened && continuity == StartContinuity::Fresh)
            return EXIT_FAILURE;
        if (listened && tls)
        {
            auto const boundPort = (*listened)->boundPort();
            nativeTcpServer.emplace(
                loop,
                "native-tcp",
                std::move(*listened),
                makeTlsNativeHandler(
                    loop,
                    host,
                    std::move(tls->context),
                    daemonNativeServices(
                        runtime, controlService, config, acceptedTokensOf(*config.nativeTcp))));
            servers.push_back(&*nativeTcpServer);
            if (tls->generated)
                tlsIdentities.insert_or_assign("native-tcp", *std::move(tls->generated));
            daemonLog()("native TCP listener on {}:{}", config.nativeTcp->host, boundPort);
        }
        else
            unserved("native-tcp");
    }

    inherited.closeUnclaimed();

    // Every listener is bound: each crosses an upgrade under its name, with the identity the daemon made up
    // for it. Kept only by an upgrade, which hands it on; otherwise nothing keeps a private key past this
    // point.
    if (!config.liveUpgrade)
        tlsIdentities.clear();
    else
        liveUpgrade.emplace(
            loop,
            host,
            config.liveUpgrade->store,
            config.liveUpgrade->leases,
            config.liveUpgrade->handoff,
            config.liveUpgrade->probe,
            upgradeHooks,
            servers,
            liveUpgradeConfigOf(config, *config.liveUpgrade, lineage, std::move(tlsIdentities)));

#ifdef _WIN32
    daemonLog()("serving on {} (native: {})", core::pathToUtf8(config.socketPath), nativePath);
#else
    daemonLog()(
        "serving on {} (native: {}, tmux: {})", core::pathToUtf8(config.socketPath), nativePath, imsgPath);
#endif
    if (servers.empty())
    {
        // Only a daemon that took over hosted programs gets here: they run on, unreachable, until it is
        // asked to end.
        errorLog()("no endpoint could be served; the adopted programs run on until the daemon is stopped");
        loop.blockOn(awaitShutdown(&loop));
    }
    else
        loop.blockOn(serveAll(servers));
    // The acceptors are destroyed before the drain below turns the loop again, and a shutdown request
    // still queued may run in it — one that finds the funnel Serving when every listener died on its own.
    // Emptied, the list lets such a request close nothing that is gone. So does an upgrade under way, which
    // the drain unwinds: its rollback must resume no listener that is gone.
    servers.clear();
    if (liveUpgrade)
        liveUpgrade->forgetListeners();

    daemonLog()("shut down");
    return EXIT_SUCCESS;
}

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
    return std::make_unique<NativeDaemonSpawner>(std::nullopt);
}

std::unique_ptr<DaemonSpawner> makeNativeDaemonSpawner(std::vector<std::string> environment)
{
    return std::make_unique<NativeDaemonSpawner>(std::move(environment));
}

std::vector<std::string> daemonEnvironment(std::span<std::string_view const> inherited,
                                           std::span<std::string const> clientOnly)
{
    auto withheld = std::vector<std::string>(clientOnly.begin(), clientOnly.end());
    std::ranges::transform(ClientActivationVariables, std::back_inserter(withheld), [](auto name) {
        return std::string { name };
    });
    return vtpty::buildEnvironmentBlock(inherited, {}, withheld, vtpty::PlatformEnvironmentNames);
}

#ifndef _WIN32
vtpty::DetachedCommand daemonDetachedCommand(std::span<std::string const> argv,
                                             std::optional<std::vector<std::string>> environment,
                                             core::Environment const& processEnvironment,
                                             core::platform::FileSystem const& fileSystem)
{
    Require(!argv.empty());
    // Resolved here, not in the child, which may only call async-signal-safe functions before it
    // execs: a PATH walk is not one. argv[0] itself stays as the caller named it -- that is what
    // the daemon's `ps` line and its own commandLine() replay should show. Looked up on the PATH the
    // daemon runs under: the given environment's, else this process's through core::Environment, not
    // getenv -- this runs on a client that already has a live reactor thread.
    auto const searchPath =
        environment ? std::string { vtpty::environmentValue(
                                        *environment, "PATH", vtpty::EnvironmentNames::CaseSensitive)
                                        .value_or(std::string_view {}) }
                    : processEnvironment.get("PATH").value_or("");
    auto image = vtpty::resolveExecutablePath(argv.front(), searchPath, fileSystem);

    // Detached means detached from the SESSION, not merely from the parent's lifetime -- this is
    // the POSIX half of what Windows gets from DETACHED_PROCESS. Without it the daemon stays
    // in the invoking shell's session and foreground process group, so Ctrl-C in that terminal
    // delivers SIGINT to the whole group and the daemon's own handler shuts down every hosted
    // session it exists to keep alive; closing the terminal is worse, because SIGHUP is not among
    // the signals runDaemon blocks and its default action kills the process outright, leaving the
    // socket file behind unbound. startDetached() gives it a session of its own.
    //
    // Its standard streams are /dev/null: holding the shell's terminal open would keep it from
    // being released, and let the daemon write into a terminal that has moved on. Its
    // diagnostics go to --log-file, which the backgrounding path always supplies.
    return vtpty::DetachedCommand { .image = std::move(image),
                                    .argv = std::vector<std::string>(argv.begin(), argv.end()),
                                    .standardInput = "/dev/null",
                                    .output = vtpty::DetachedOutput::Discarded,
                                    .role = vtpty::ProcessRole::Daemon,
                                    .environment = std::move(environment) };
}
#endif

int ensureDaemon(AttachEndpoint const& endpoint,
                 DaemonBinarySource& binary,
                 DaemonSpawner& spawner,
                 vtpty::ProcessPlacement& placement,
                 DaemonSpawnOptions const& spawnOptions,
                 std::chrono::seconds timeout,
                 DaemonLifecycle lifecycle)
{
    // TCP endpoints: no auto-spawn (can't spawn a remote daemon).
    if (std::holds_alternative<TcpEndpoint>(endpoint))
        return EXIT_SUCCESS;

    // Unix socket: probe the native socket beside the control socket.
    auto const& unixEp = std::get<UnixEndpoint>(endpoint);
    if (spawner.probe(unixEp.socketPath) == DaemonPresence::Serving)
        return EXIT_SUCCESS; // daemon is already running

    // Daemon not running — spawn it in the background. A daemon we spawn is ours, so by default it is started
    // `--lifecycle=exit-when-idle` and ends once nothing needs it any more; one the user named is persistent.
    // The image is asked for only now, and held until the daemon serves: by then the daemon has leased it
    // itself.
    auto const image = binary.imageForSpawn();
    auto const args =
        daemonSpawnArgs(core::pathToUtf8(image.executable), unixEp.socketPath, spawnOptions, lifecycle);
    return spawnAndAwaitDaemon(spawner, args, placement, unixEp.socketPath, timeout);
}

int ensureDaemon(AttachEndpoint const& endpoint,
                 DaemonBinarySource& binary,
                 vtpty::ProcessPlacement& placement,
                 DaemonSpawnOptions const& spawnOptions,
                 std::chrono::seconds timeout,
                 DaemonLifecycle lifecycle)
{
    return ensureDaemon(
        endpoint, binary, *makeNativeDaemonSpawner(), placement, spawnOptions, timeout, lifecycle);
}

int runDaemonDetached(std::vector<std::string> commandLine,
                      std::filesystem::path const& socketPath,
                      vtpty::ProcessPlacement& placement,
                      std::chrono::seconds timeout)
{
    return spawnAndAwaitDaemon(*makeNativeDaemonSpawner(), commandLine, placement, socketPath, timeout);
}

} // namespace vthost
