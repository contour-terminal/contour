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
#include <vtbackend/screen/GitHead.hpp>

#include <vtpty/DetachedProcess.hpp>
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/ProgramLookup.hpp>
#include <vtpty/WindowsCommandLine.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Random.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/WideString.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
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
#include <exception>
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
#include <vthost/DaemonIdentity.hpp>
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
#include <vthost/keeper/Messages.hpp>
#include <vthost/tmux/ControlSession.hpp>
#include <vthost/tmux/ImsgServer.hpp>

#ifndef _WIN32
    #include <core/net/posix/UnixListener.hpp>

    #include <fcntl.h>
    #include <unistd.h>

    #include <vthost/keeperlink/KeeperLink.hpp>
    #include <vthost/keeperlink/KeeperPtyFactory.hpp>
    #include <vthost/keeperlink/Recovery.hpp>
    #include <vthost/keeperlink/Structure.hpp>
#endif

namespace vthost
{

namespace
{
    /// The socket file of each endpoint the daemon serves on one, by the endpoint's name.
    using SocketPaths = std::map<std::string, std::string, std::less<>>;

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

    /// Makes the device a hosted shell runs on, at the size given: its process on a fresh PTY, whoever is its
    /// parent. Shared by the daemon's PTY factories, so that one decides who spawns for both.
    using ShellDevices = std::function<std::unique_ptr<vtpty::Pty>(
        vtbackend::PageSize, vtpty::Process::ExecInfo, SandboxEscape)>;

    /// @param placement Moves each hosted shell into its own resource domain.
    /// @return Shell devices this daemon spawns itself, each a child of its own (vtpty::Process).
    [[nodiscard]] ShellDevices ownShellDevices(std::shared_ptr<vtpty::ProcessPlacement> placement)
    {
        return [placement = std::move(placement)](vtbackend::PageSize pageSize,
                                                  vtpty::Process::ExecInfo const& exec,
                                                  SandboxEscape sandbox) -> std::unique_ptr<vtpty::Pty> {
            return std::make_unique<vtpty::Process>(
                exec, vtpty::createPty(pageSize, std::nullopt), sandbox == SandboxEscape::Escapes, placement);
        };
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
    /// @param devices       Makes each shell's device: spawned by this daemon, or by its PTY keeper.
    [[nodiscard]] PtyFactory makeShellPtyFactory(vtpty::Process::ExecInfo shell,
                                                 bool escapeSandbox,
                                                 std::filesystem::path const& socketPath,
                                                 ShellDevices devices)
    {
        return [shell = std::move(shell),
                hosted = hostedShellEnvironment(socketPath),
                sandbox = vtpty::sandboxEscapeFromConfig(escapeSandbox),
                devices = std::move(devices)](vtbackend::PageSize pageSize,
                                              std::optional<vtpty::Process::ExecInfo> const& commandOverride)
                   -> std::unique_ptr<vtpty::Pty> {
            // A fresh shell per spawn: `shell` is captured by the closure and reused for every session
            // this factory spawns, so writing one session's override back into it would leak that pane's
            // command/directory into every later session. Mirrors AppSessionFactory::createPty.
            return devices(
                pageSize, hostedShellFor(shell, commandOverride, ShellOverride::Overlays, hosted), sandbox);
        };
    }

    /// The PTY factory of a daemon with profiles: each session runs its resolved profile's whole shell, with
    /// that profile's own environment and sandbox policy, as the in-process GUI spawns that profile
    /// (SessionFactory takes `escape_sandbox` from the session's own profile).
    /// @param shell      The daemon's default shell, whose program runs when a profile names none.
    /// @param socketPath The daemon's control socket, which every hosted shell is told about.
    /// @param devices    Makes each shell's device: spawned by this daemon, or by its PTY keeper.
    [[nodiscard]] ProfilePtyFactory makeProfilePtyFactory(vtpty::Process::ExecInfo shell,
                                                          std::filesystem::path const& socketPath,
                                                          ShellDevices devices)
    {
        return [base = std::move(shell),
                hosted = hostedShellEnvironment(socketPath),
                devices = std::move(devices)](vtbackend::PageSize pageSize,
                                              vtpty::Process::ExecInfo const& profileShell,
                                              SandboxEscape sandbox) -> std::unique_ptr<vtpty::Pty> {
            return devices(
                pageSize, hostedShellFor(base, profileShell, ShellOverride::Replaces, hosted), sandbox);
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
        /// The certificate's, as OpenSSH spells one; if it cannot be read, why, as `contour daemon status`
        /// shows it.
        std::string fingerprint;
    };

    /// @param identity The certificate the TCP listener presents.
    /// @return Its fingerprint; else why not, logged, in words `contour daemon status` can show.
    [[nodiscard]] std::string statedFingerprint(handover::TlsIdentity const& identity)
    {
        auto fingerprint = daemonFingerprint(identity);
        if (fingerprint)
            return *std::move(fingerprint);
        errorLog()("cannot state the TLS certificate's fingerprint: {}", fingerprint.error());
        return std::format("unavailable ({})", fingerprint.error());
    }

    /// Builds the TLS server context for the native TCP listener, TLS 1.3 only: the configured PEM cert +
    /// key; else the self-signed identity a live upgrade carried over, so that a client that trusted it on
    /// first use goes on trusting it; else the daemon's own persistent identity file, made on first use, so
    /// that its fingerprint survives a restart; else an ephemeral self-signed one, made up now. The TCP
    /// transport is always encrypted.
    /// @param config The listener's configuration.
    /// @param carried The identity the previous image of this daemon presented on it; nullopt for none.
    /// @param fileSystem What the identity file is read and written through.
    /// @param privacy What makes the identity file private.
    /// @return The TLS, or nullopt after logging why it could not be built.
    [[nodiscard]] std::optional<NativeTcpTls> makeNativeTcpTls(NativeTcpListenerConfig const& config,
                                                               std::optional<handover::TlsIdentity> carried,
                                                               core::platform::FileSystem const& fileSystem,
                                                               IFilePrivacy const& privacy)
    {
        if (config.tlsCertPath.empty() && config.tlsKeyPath.empty())
        {
            if (carried)
            {
                if (auto context = core::net::makeTlsServerContext(
                        carried->certPem, carried->keyPem, NativeTcpTlsOptions))
                {
                    daemonLog()(
                        "native TCP keeps the self-signed certificate it presented before the upgrade");
                    auto fingerprint = statedFingerprint(*carried);
                    return NativeTcpTls { .context = *std::move(context),
                                          .generated = std::move(carried),
                                          .fingerprint = std::move(fingerprint) };
                }
                else
                    errorLog()("cannot present the carried TLS certificate again ({}); clients that trusted "
                               "it will be asked to trust a new one",
                               context.error());
            }
            if (!config.identityFile.empty())
            {
                auto identity = loadOrCreateDaemonIdentity(fileSystem, privacy, config.identityFile);
                auto context = identity.and_then([](handover::TlsIdentity const& pem) {
                    return core::net::makeTlsServerContext(pem.certPem, pem.keyPem, NativeTcpTlsOptions);
                });
                if (!context)
                {
                    errorLog()("cannot present the daemon's TLS identity: {}", context.error());
                    return std::nullopt;
                }
                daemonLog()("native TCP presents the daemon's identity from '{}'",
                            core::pathToUtf8(config.identityFile));
                return NativeTcpTls { .context = *std::move(context),
                                      .generated = std::nullopt,
                                      .fingerprint = statedFingerprint(*identity) };
            }
            auto material = core::net::generateSelfSignedCertificate({ .commonName = "contour-daemon" });
            auto context = material.and_then([](core::net::CertKeyPem const& pem) {
                return core::net::makeTlsServerContext(pem.certPem, pem.keyPem, NativeTcpTlsOptions);
            });
            if (!context)
            {
                errorLog()("cannot build an ephemeral TLS certificate: {}", context.error());
                return std::nullopt;
            }
            daemonLog()("native TCP is using an ephemeral self-signed certificate (TOFU)");
            auto identity = handover::TlsIdentity { .certPem = std::move(material->certPem),
                                                    .keyPem = std::move(material->keyPem) };
            auto fingerprint = statedFingerprint(identity);
            return NativeTcpTls { .context = *std::move(context),
                                  .generated = std::move(identity),
                                  .fingerprint = std::move(fingerprint) };
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
        auto context = core::net::makeTlsServerContext(*cert, *key, NativeTcpTlsOptions);
        if (!context)
        {
            errorLog()("cannot build the TLS server context: {}", context.error());
            return std::nullopt;
        }
        return NativeTcpTls { .context = *std::move(context),
                              .generated = std::nullopt,
                              .fingerprint = statedFingerprint(
                                  handover::TlsIdentity { .certPem = *cert, .keyPem = {} }) };
    }

    /// The TCP listener's handler: TLS with @p tls's context, then the native protocol.
    /// @param loop The loop every connection runs on.
    /// @param host The session host every connection serves.
    /// @param tls The listener's TLS, whose context is taken; nullopt when it could not be had.
    /// @param services What each connection is served with.
    /// @param tcp The listener.
    /// @return The handler; nullopt, with why logged, when the listener is not to be served.
    [[nodiscard]] std::optional<ConnectionHandler> nativeTcpHandler(core::net::EventLoop& loop,
                                                                    SessionHost& host,
                                                                    std::optional<NativeTcpTls>& tls,
                                                                    NativeSessionServices services,
                                                                    NativeTcpListenerConfig const& tcp)
    {
        if (!tls)
            return std::nullopt;
        auto made = makeTlsNativeHandler(
            loop,
            host,
            std::move(tls->context),
            std::move(services),
            TlsKeyAuth { .keys = tcp.authorizedKeys, .auth = tcp.auth, .random = core::systemRandom() });
        if (made)
            return *std::move(made);
        errorLog()("native TCP listener on {}:{} is not served: {}", tcp.host, tcp.port, made.error());
        return std::nullopt;
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
    /// @return What the first client is told of an endpoint a daemon serves on without after it took over
    ///         running sessions (a live upgrade, a restart after a crash).
    [[nodiscard]] std::string unservedEndpointNotice(std::string_view endpoint)
    {
        return std::format("Contour could not serve its {} endpoint when the daemon took over its running "
                           "sessions, and runs on "
                           "without it; "
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

    /// @param listener The TCP listener's configuration; nullopt for none.
    /// @param inherited What the previous image handed over, among it the listener's carried identity.
    /// @param fileSystem What the identity file is read and written through.
    /// @param privacy What makes the identity file private.
    /// @return The listener's TLS (makeNativeTcpTls); nullopt without a listener, or when it cannot be built.
    [[nodiscard]] std::optional<NativeTcpTls> nativeTcpTlsOf(
        std::optional<NativeTcpListenerConfig> const& listener,
        InheritedListeners& inherited,
        core::platform::FileSystem const& fileSystem,
        IFilePrivacy const& privacy)
    {
        return listener.and_then([&](NativeTcpListenerConfig const& config) {
            return makeNativeTcpTls(config, inherited.takeTlsIdentity("native-tcp"), fileSystem, privacy);
        });
    }

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
    /// @param image The handover image it starts from, or the structure its keeper kept; nullopt for a daemon
    ///        started afresh.
    /// @param continuity How it started.
    /// @return The run it serves as: the image's, one upgrade further; the structure's, as it was (a restart
    ///         after a crash is no upgrade); or a fresh run of @p config's.
    [[nodiscard]] DaemonLineage lineageOf(DaemonConfig const& config,
                                          std::optional<handover::HandoverImage> const& image,
                                          StartContinuity continuity)
    {
        if (!image || image->daemonInstance == 0)
            return DaemonLineage { .instance = config.instance, .upgradeGeneration = 0 };
        switch (continuity)
        {
            case StartContinuity::Recovered:
                return DaemonLineage { .instance = image->daemonInstance,
                                       .upgradeGeneration = image->upgradeGeneration };
            case StartContinuity::Fresh:
            case StartContinuity::HandedOver: break;
        }
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
        std::map<std::string, handover::TlsIdentity, std::less<>> tlsIdentities,
        std::vector<vtpty::CrossingHandle> alsoCrossing)
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
                                             .tlsIdentities = std::move(tlsIdentities),
                                             .alsoCrossing = std::move(alsoCrossing) };
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
                if (carriesOn(continuity) && host.sessionCount() == 0)
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

    /// What a restart after a crash takes from the snapshot, which it reads and leaves as it is.
    struct RecoveredSnapshot
    {
        RecoveredSeeder seeder; ///< Seeds each recovered session; null for none.
        PendingRestoreCopy pendingCopy = PendingRestoreCopy::Kept; ///< What becomes of a `.restored` it read.
    };

    /// @param continuity How the daemon started.
    /// @param store The snapshot file, while persistence is on.
    /// @param config How the daemon keeps its workspace.
    /// @param wall The daemon's wall clock, which dates the divider.
    /// @return For a restart after a crash, what seeds each recovered session from the snapshot, and whether
    ///         the first write drops the `.restored` copy it read (left by a start that died before it wrote
    ///         one); nothing for any other start, or without a snapshot.
    [[nodiscard]] RecoveredSnapshot recoveredSnapshotOf(
        [[maybe_unused]] StartContinuity continuity,
        [[maybe_unused]] std::optional<SessionSnapshotStore> const& store,
        [[maybe_unused]] WorkspacePersistenceConfig const& config,
        [[maybe_unused]] core::platform::WallClockRef wall)
    {
#ifndef _WIN32
        if (continuity == StartContinuity::Recovered && store)
            if (auto peeked = peekRestore(*store, config, logPersistence, wall))
            {
                auto const pendingCopy = peeked->source == RestoreSource::PendingRestore
                                             ? PendingRestoreCopy::DroppedAfterFirstWrite
                                             : PendingRestoreCopy::Kept;
                return RecoveredSnapshot { .seeder = keeperlink::snapshotSeederOf(std::move(*peeked)),
                                           .pendingCopy = pendingCopy };
            }
#endif
        return {};
    }

#ifndef _WIN32
    /// @param sink Where the structure goes; null behind no keeper.
    /// @param identities The TLS identities the daemon made up for its listeners, by listener name.
    /// @return Them, as the structure its keeper keeps names them: by listener, without a handle; none behind
    ///         no keeper, so that nothing but an upgrade that hands them on keeps the private keys.
    [[nodiscard]] std::vector<handover::InheritedListener> keptIdentitiesOf(
        keeperlink::StructureSink const* sink,
        std::map<std::string, handover::TlsIdentity, std::less<>> const& identities)
    {
        auto listeners = std::vector<handover::InheritedListener> {};
        if (sink == nullptr)
            return listeners;
        for (auto const& [name, identity]: identities)
            listeners.push_back(
                handover::InheritedListener { .name = name, .handle = {}, .tlsIdentity = identity });
        return listeners;
    }
#endif

    class BoundPersistence;

    /// What a daemon does before its listeners close, in order: the workspace's last snapshot, the keeper's
    /// announcement (a clean end, not a crash to restart from), the caller's beforeShutdown, then the word
    /// that lets a console-control handler holding the process's end return (ShutdownSignals::watch's
    /// lastWrite). Built before the funnel that runs it and the signal watch that
    /// waits on it; the persistence it finalizes exists only once the host does, so a BoundPersistence binds
    /// itself in for its own lifetime.
    class ShutdownSequence
    {
      public:
        /// @param callerHook DaemonConfig::beforeShutdown (not owned; outlives this); may be empty.
        /// @param announce Tells the keeper the daemon shuts down; empty for a daemon behind none.
        ShutdownSequence(std::function<void(ShutdownCause)> const& callerHook,
                         std::function<void(ShutdownCause)> announce):
            _callerHook { callerHook },
            _announce { std::move(announce) },
            _lastWrite { _lastWritten.get_future().share() }
        {
        }

        /// Runs the sequence. Loop thread; once, as the funnel runs its hook.
        /// @param cause What ends the daemon.
        void run(ShutdownCause cause)
        {
            if (_persistence != nullptr)
                _persistence->finalize(cause);
            if (_announce)
                _announce(cause);
            if (_callerHook)
                _callerHook(cause);
            _lastWritten.set_value();
        }

        /// @return Ready once run() is done.
        [[nodiscard]] std::shared_future<void> lastWrite() const { return _lastWrite; }

      private:
        friend class BoundPersistence;

        std::function<void(ShutdownCause)> const& _callerHook;
        std::function<void(ShutdownCause)> _announce;
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
                         core::platform::IWallClock const& wallClock,
                         PendingRestoreCopy pendingCopy):
            _persistence(loop,
                         host,
                         store,
                         restoredFrom,
                         config,
                         wallClock,
                         WriterThreading::Background,
                         logPersistence,
                         pendingCopy),
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
    /// @param tlsFingerprint The fingerprint its TCP listener presents; empty without a listener.
    /// @param keeperProcessId The PTY keeper it runs behind; 0 for none.
    /// @return What its DaemonRuntime states about it.
    [[nodiscard]] DaemonRuntimeConfig runtimeConfigOf(DaemonConfig const& config,
                                                      uint64_t upgradeGeneration,
                                                      std::string tlsFingerprint,
                                                      uint64_t keeperProcessId)
    {
        return DaemonRuntimeConfig { .releaseVersion = config.releaseVersion,
                                     .processId = currentProcessId(),
                                     .socketPath = config.socketPath,
                                     .executablePath = config.executablePath,
                                     .logFile = config.logFile,
                                     .lifecycle = config.lifecycle,
                                     .upgradeGeneration = upgradeGeneration,
                                     .relocationRequest = config.relocationRequest,
                                     .tlsFingerprint = std::move(tlsFingerprint),
                                     .keeperProcessId = keeperProcessId };
    }

#ifndef _WIN32
    /// Tells @p link every KeeperHeartbeatInterval that the loop thread is alive, until the loop stops.
    /// @param loop The daemon's loop (not owned; outlives the flow).
    /// @param link The keeper's link (not owned; outlives the flow).
    core::async::Task<void> keepTellingTheKeeper(core::net::EventLoop* loop, keeperlink::KeeperLink* link)
    {
        try
        {
            while (true)
            {
                co_await loop->delay(KeeperHeartbeatInterval);
                link->post(keeper::Heartbeat {});
            }
        }
        catch (core::async::OperationCancelled const&)
        {
            co_return;
        }
    }

    /// @param label The tmux-compat label (DaemonConfig::tmuxCompatLabel).
    /// @return tmux's own discovery path for it, which the tmux-compat endpoint binds.
    [[nodiscard]] std::string tmuxCompatPathOf(std::string const& label)
    {
        return std::format("/tmp/tmux-{}/{}", ::getuid(), label);
    }

    /// Tells the keeper what it keeps of a daemon that serves now: a duplicate of every listener in @p
    /// servers, each with its socket file, then that it serves.
    /// @param link The keeper's link (not owned; outlives the loop's flows).
    /// @param config The daemon's configuration: its keeper settings.
    /// @param servers Every endpoint the daemon serves.
    /// @param paths The socket file of each endpoint in @p servers that has one, by its name.
    void tellTheKeeper(keeperlink::KeeperLink& link,
                       DaemonConfig const& config,
                       std::span<ConnectionAcceptor* const> servers,
                       SocketPaths const& paths)
    {
        for (auto const* server: servers)
        {
            auto const handle = server->listeningHandle();
            if (handle.isNone())
                continue;
            auto const path = paths.find(server->name());
            link.post(keeper::Listener { .name = server->name(),
                                         .tlsIdentity = std::nullopt,
                                         .path = path != paths.end() ? path->second : std::string {} },
                      { handle.native() });
        }
        link.post(keeper::Serving { .hungRestart = config.keeper.hungRestart,
                                    .heartbeatTimeoutMs =
                                        static_cast<uint32_t>(config.keeper.heartbeatTimeout.count()) });
    }
#endif

    /// A daemon's side of the PTY keeper it runs behind, for as long as runDaemon() runs; empty for a daemon
    /// behind none, and on Windows, where there is no keeper yet.
    ///
    /// Built from the handshake's attachment first, linked once the shutdown funnel it reports a loss to
    /// exists: the funnel's sequence announces the shutdown through it, and it requests the shutdown from the
    /// funnel when the keeper is gone (cyclic wiring, one link() call).
    class DaemonKeeper
    {
      public:
        /// @param attachment The keeper's channel past the handshake; nullopt for none.
        explicit DaemonKeeper(std::optional<keeperlink::KeeperAttachment> attachment):
            _attachment { std::move(attachment) }
        {
        }

        /// @return How the keeper launched this daemon process; Fresh for one behind none.
        [[nodiscard]] keeper::LaunchContinuity continuity() const noexcept
        {
            return _attachment ? _attachment->continuity : keeper::LaunchContinuity::Fresh;
        }

        /// @return The keeper's process id; 0 for none.
        [[nodiscard]] uint64_t processId() const noexcept
        {
            return _attachment ? static_cast<uint64_t>(_attachment->keeperProcessId) : 0;
        }

        /// @return What tells the keeper that the daemon shuts down; empty for a daemon behind none.
        [[nodiscard]] std::function<void(ShutdownCause)> announcer()
        {
            if (!_attachment)
                return {};
            return [this](ShutdownCause cause) {
#ifndef _WIN32
                // Waited for: the keeper lets go of the label before the listeners close, so a start that
                // follows the stop at once finds the label free.
                if (_link)
                    _link->announceShutdown(std::to_underlying(cause));
#else
                std::ignore = cause;
#endif
            };
        }

        /// Starts the link on @p loop, and for a Recovered start rebuilds what the keeper kept
        /// (keeperlink::imageFromInventory()). Called once.
        /// @param loop The daemon's loop (not owned; outlives this).
        /// @param onLost Called on the loop when the keeper is gone.
        void link([[maybe_unused]] core::net::EventLoop& loop, [[maybe_unused]] std::function<void()> onLost)
        {
#ifndef _WIN32
            if (!_attachment)
                return;
            if (_attachment->continuity == keeper::LaunchContinuity::Recovered)
            {
                _recovery = keeperlink::imageFromInventory(_attachment->inventory, *_memory);
                for (auto const& problem: _recovery->problems)
                    errorLog()("recovery: {}{}",
                               keeperlink::describe(problem.error),
                               problem.session != 0 ? std::format(" (keeper session {})", problem.session)
                                                    : std::string {});
            }
            _link.emplace(loop, std::move(_attachment->channel), _attachment->version, std::move(onLost));
            _attachment->inventory.clear();
            // Alive from here on, not only once it serves: a start is judged by its heartbeats once the first
            // came, so that one restoring at length is not taken for hung. The loop beats once it turns; the
            // setup before that beats at its milestones (beat()).
            _link->beat();
            loop.spawn(keepTellingTheKeeper(&loop, &*_link));
#endif
        }

        /// Seals the link (keeperlink::KeeperLink::seal()) as it is destroyed.
        class Seal
        {
          public:
            explicit Seal(DaemonKeeper& keeper) noexcept: _keeper { keeper } {}
            Seal(Seal const&) = delete;
            Seal(Seal&&) = delete;
            Seal& operator=(Seal const&) = delete;
            Seal& operator=(Seal&&) = delete;
            ~Seal() { _keeper.seal(); }

          private:
            DaemonKeeper& _keeper;
        };

        /// @return What seals the link as runDaemon() goes: declared right after the host, it is destroyed
        ///         before it, so that an end the daemon did not announce sends none of the Releases the
        ///         host's sessions post as they go.
        [[nodiscard]] Seal sealAtExit() noexcept { return Seal { *this }; }

        /// Seals the link unless the daemon announced its shutdown; nothing behind no keeper.
        void seal()
        {
#ifndef _WIN32
            if (_link)
                _link->seal();
#endif
        }

        /// Tells the keeper at once that the daemon is alive: a milestone of the setup that runs before the
        /// loop turns and its heartbeat flow with it. Nothing behind no keeper.
        void beat()
        {
#ifndef _WIN32
            if (_link)
                _link->beat();
#endif
        }

        /// @param placement Moves each hosted shell into its own resource domain.
        /// @return What hosted shells run on: spawned through the keeper behind one, by this daemon
        ///         otherwise. Called once: the keeper's factory takes over the link's Exited handler.
        [[nodiscard]] ShellDevices shellDevices(std::shared_ptr<vtpty::ProcessPlacement> placement)
        {
#ifndef _WIN32
            if (_link)
            {
                _placement = placement;
                _factory = std::make_shared<keeperlink::KeeperPtyFactory>(*_link, std::move(placement));
                return [factory = _factory](vtbackend::PageSize pageSize,
                                            vtpty::Process::ExecInfo exec,
                                            SandboxEscape sandbox) -> std::unique_ptr<vtpty::Pty> {
                    return factory->make(pageSize, std::move(exec), sandbox);
                };
            }
#endif
            return ownShellDevices(std::move(placement));
        }

        /// @return The image a Recovered start serves from (keeperlink::Recovery::image); nullopt for any
        ///         other start.
        [[nodiscard]] std::optional<handover::HandoverImage> takeRecoveredImage()
        {
#ifndef _WIN32
            if (_recovery)
                return std::exchange(_recovery->image, {});
#endif
            return std::nullopt;
        }

        /// @return What binds each session a handover image or a recovery names as a keeper's child to its
        ///         child again (SessionHostConfig::adoptChild); null behind no keeper. After shellDevices().
        [[nodiscard]] KeeperChildAdopter childAdopter()
        {
#ifndef _WIN32
            if (_factory)
                return [this](handover::HandoverSession const& session, std::unique_ptr<vtpty::Pty> pty) {
                    return adoptChild(session, std::move(pty));
                };
#endif
            return {};
        }

        /// Releases every session the keeper kept that the host did not adopt: nothing of this daemon holds
        /// it, so the keeper hangs it up. After the host is built.
        void releaseUnadopted()
        {
#ifndef _WIN32
            if (!_recovery || !_link)
                return;
            auto released = std::exchange(_recovery->unbound, {});
            for (auto const& [session, child]: _recovery->children)
                released.push_back(keeper::KeeperSessionId { session });
            _recovery->children.clear();
            for (auto const session: released)
                _link->post(keeper::Release {
                    .session = std::to_underlying(session),
                    .graceMs = static_cast<uint32_t>(
                        std::chrono::milliseconds { vtpty::HangupGrace::Default }.count()) });
            if (!released.empty())
                errorLog()("recovery: {} session(s) the keeper kept could not be served; released",
                           released.size());
#endif
        }

        /// @return What a live upgrade's exec keeps across beside the image's handles: the keeper's channel.
        [[nodiscard]] std::vector<vtpty::CrossingHandle> crossing() const
        {
#ifndef _WIN32
            if (_link)
                return { vtpty::CrossingHandle::of(_link->channelHandle()) };
#endif
            return {};
        }

        /// Turns @p loop until @p flow is done.
        ///
        /// Behind a keeper, an exception that escapes a turn ends the process at once (std::abort()), as any
        /// other crash does. Unwinding would run what a clean stop runs: the drain turns the loop again, and
        /// the host releases every session's child, which the keeper would then hang up. Aborted, the daemon
        /// ends as a crash, without a word: the keeper keeps every session for the daemon it restarts, and
        /// the core shows where it was. Behind none, the exception propagates.
        /// @param loop The daemon's loop.
        /// @param flow What the daemon serves until it ends.
        void serve(core::net::EventLoop& loop, core::async::Task<void> flow) const
        {
            if (!_attachment)
            {
                loop.blockOn(std::move(flow));
                return;
            }
            // A log line that throws in turn (a bad_alloc after a bad_alloc) aborts too, never unwinding into
            // what the abort is there to prevent.
            auto const crash = [](std::string_view what) noexcept {
                try
                {
                    errorLog()(
                        "an exception escaped the daemon's loop ({}); ending as a crash, for the keeper "
                        "to recover every session",
                        what);
                }
                catch (...)
                {
                    std::abort();
                }
                std::abort();
            };
            try
            {
                loop.blockOn(std::move(flow));
            }
            catch (std::exception const& error)
            {
                crash(error.what());
            }
            catch (...)
            {
                crash("of unknown type");
            }
        }

        /// Tells the keeper what it keeps of a daemon that serves now (tellTheKeeper()); nothing without one.
        void startKeeping([[maybe_unused]] DaemonConfig const& config,
                          [[maybe_unused]] std::span<ConnectionAcceptor* const> servers,
                          [[maybe_unused]] SocketPaths const& paths)
        {
#ifndef _WIN32
            if (_link)
                tellTheKeeper(*_link, config, servers, paths);
#endif
        }

#ifndef _WIN32
        /// @return Where the structure goes: to the keeper, as a sealed memory object; null behind none.
        [[nodiscard]] keeperlink::StructureSink* structureSink() noexcept
        {
            if (!_link)
                return nullptr;
            if (!_sink)
                _sink.emplace(*_link, *_memory);
            return &*_sink;
        }
#endif

      private:
#ifndef _WIN32
        /// Binds @p session, which a handover image or a recovery names as a keeper's child, to its child.
        [[nodiscard]] std::unique_ptr<vtpty::Pty> adoptChild(handover::HandoverSession const& session,
                                                             std::unique_ptr<vtpty::Pty> pty)
        {
            auto recovered = std::optional<keeperlink::RecoveredChild> {};
            if (_recovery)
                if (auto found = _recovery->children.extract(session.keeperSession))
                    recovered = std::move(found.mapped());
            // A live upgrade's: the same daemon process, whose link hears the child's exit from here on.
            auto kept = keeperlink::KeptChild { .session = keeper::KeeperSessionId { session.keeperSession },
                                                .pid = session.child.pid,
                                                .exited = std::nullopt,
                                                .settle = {} };
            if (recovered)
                kept = std::move(recovered->kept);
            auto const pid = kept.pid;
            auto child = _factory->adopt(std::move(kept), std::move(pty));
            if (!recovered)
                return child;
            // A recovered terminal is fresh: its program is asked to draw it again.
            keeperlink::nudgeRedraw(*child);
            // A child the dead daemon never placed waits on its gate still: placed now, then released.
            if (recovered->gate.isOpen())
            {
                daemonLog()("recovery: keeper session {} was never placed; placing it now",
                            session.keeperSession);
                _placement->placeThenRelease(vtpty::ParkedChild { static_cast<int>(pid),
                                                                  std::move(recovered->processHandle),
                                                                  std::move(recovered->gate),
                                                                  vtpty::ProcessRole::Session });
                _link->post(keeper::Placed { .session = session.keeperSession });
            }
            return child;
        }
#endif

        std::optional<keeperlink::KeeperAttachment> _attachment;
#ifndef _WIN32
        std::unique_ptr<handover::HandoverMemory> _memory = handover::makeNativeHandoverMemory();
        std::optional<keeperlink::Recovery> _recovery;
        std::optional<keeperlink::KeeperLink> _link;
        std::shared_ptr<vtpty::ProcessPlacement> _placement;
        std::shared_ptr<keeperlink::KeeperPtyFactory> _factory;
        std::optional<keeperlink::KeeperStructureSink> _sink;
#endif
    };

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
                                                       proto::Capability::WorkspaceSwitch,
                                                       proto::Capability::DaemonFollow,
                                                       proto::Capability::WorkspaceMeta,
                                                       proto::Capability::ClientGoodbye });
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
    ///
    /// The connect runs on a reactor of its own, made for this one probe: a loop is driven by exactly one
    /// thread (core::net's G1), and a probe is a blocking call any thread may make, several at once.
    /// @param path The native socket file to probe.
    /// @return True if a daemon accepted the connection.
    [[nodiscard]] bool daemonAccepts(std::string const& path)
    {
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
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

    /// The operating system's DaemonSpawner. Holds no reactor: each probe drives its own, so probes may run
    /// on several threads at once.
    class NativeDaemonSpawner final: public DaemonSpawner
    {
      public:
        /// @param environment What every daemon it spawns runs under; std::nullopt for this process's own.
        explicit NativeDaemonSpawner(std::optional<std::vector<std::string>> environment):
            _environment { std::move(environment) }
        {
        }

        [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                             vtpty::ProcessPlacement& placement) override
        {
            return spawnDetachedDaemon(argv, _environment, placement);
        }

        [[nodiscard]] DaemonPresence probe(std::filesystem::path const& socketPath) override
        {
            return daemonAccepts(core::pathToUtf8(nativeSocketPath(socketPath))) ? DaemonPresence::Serving
                                                                                 : DaemonPresence::Absent;
        }

      private:
        std::optional<std::vector<std::string>> _environment;
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
    // Keys alone: a token, however configured, admits nobody.
    if (listener.auth == DaemonAuth::KeysOnly)
        return tokens;
    if (!listener.token.empty())
        tokens.push_back(
            AcceptedToken { .token = listener.token, .grants = vtcontrol::ControlGrants::all() });
    tokens.insert(tokens.end(), listener.accessTokens.begin(), listener.accessTokens.end());
    return tokens;
}

namespace
{
    /// @param spec `HOST:PORT`, or `[v6]:PORT`.
    /// @param lowestPort The smallest port @p spec may name.
    /// @return The host and port, or nullopt if @p spec is malformed or names a lower port.
    std::optional<std::pair<std::string, std::uint16_t>> parseHostPortFrom(std::string_view spec,
                                                                           unsigned lowestPort)
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
        if (ec != std::errc {} || ptr != last || port < lowestPort || port > 65535)
            return std::nullopt;
        return std::pair { std::string { host }, static_cast<std::uint16_t>(port) };
    }
} // namespace

std::optional<std::pair<std::string, std::uint16_t>> parseHostPort(std::string_view spec)
{
    return parseHostPortFrom(spec, 1);
}

std::optional<std::pair<std::string, std::uint16_t>> parseListenSpec(std::string_view spec)
{
    return parseHostPortFrom(spec, 0);
}

StartContinuity startContinuityOf(std::optional<handover::HandoverImage> const& image,
                                  keeper::LaunchContinuity launch) noexcept
{
    if (image)
        return StartContinuity::HandedOver;
    switch (launch)
    {
        case keeper::LaunchContinuity::Recovered: return StartContinuity::Recovered;
        case keeper::LaunchContinuity::Fresh:
        case keeper::LaunchContinuity::HandedOver: break; // without an image, nothing is carried on
    }
    return StartContinuity::Fresh;
}

int runDaemon(DaemonConfig const& config,
              std::optional<handover::HandoverImage> image,
              std::optional<keeperlink::KeeperAttachment> keeper)
{
    Require(config.shutdownSignals != nullptr);
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const clock = core::platform::SteadyClock {};
    auto const wallClock = core::platform::SystemWallClock {};

    // The keeper this daemon runs behind, if any: it outlives the drain, so that the Shutdown the sequence
    // posts as the last flows unwind is still sent.
    auto daemonKeeper = DaemonKeeper { std::move(keeper) };
    // How this start began: a live upgrade's handover, a keeper's restart after a crash, or afresh.
    auto const continuity = startContinuityOf(image, daemonKeeper.continuity());

    // Filled as each endpoint is bound below; the funnel reads it when the shutdown comes.
    auto servers = std::vector<ConnectionAcceptor*> {};
    auto sequence = ShutdownSequence { config.beforeShutdown, daemonKeeper.announcer() };
    auto funnel = ShutdownFunnel { loop, servers, [&sequence](ShutdownCause cause) { sequence.run(cause); } };
    daemonKeeper.link(loop, [&funnel] { funnel.request(ShutdownCause::KeeperLost); });
    // A restart after a crash serves from what its keeper kept, as a handed-over daemon serves from its
    // image.
    if (continuity == StartContinuity::Recovered)
        image = daemonKeeper.takeRecoveredImage();

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
    // A restart after a crash reads the snapshot and changes nothing of it, so that no crash loop sets it
    // aside: only for the history of the sessions it recovers, which no checkpoint carries yet.
    auto recovery = recoveredSnapshotOf(continuity, store, config.persistence, wallClock);
    // The same run of the daemon goes on across a live upgrade, one upgrade further, and across a crash.
    auto const lineage = lineageOf(config, image, continuity);

    // The workspaces' git state: looked up by a `git` this daemon starts on a thread of its own, so a slow
    // repository stalls the sidebar's branch and nothing else. Declared before the host, so the host goes
    // first and the executor is joined next, while the probe it runs is still there. Started here, on the
    // thread building the host, whose signal mask the thread inherits (as HostGitLookups does).
    auto const gitFileSystem =
        vtbackend::NativeGitFileSystem { core::platform::NativeFileSystem::instance() };
    auto const gitCommands = makeNativeCommandRunner(gitProbeEnvironment(core::defaultEnvironment()));
    auto const gitProbe = makeGitStatusProbe(gitFileSystem, *gitCommands);
    auto gitExecutor = core::async::ThreadPoolExecutor { 1 };

    // Behind a keeper, every shell is the keeper's child, spawned through it; otherwise this daemon's own.
    auto const shellDevices = daemonKeeper.shellDevices(config.placement);
    auto host = SessionHost {
        loop,
        makeShellPtyFactory(config.shell, config.escapeSandbox, config.socketPath, shellDevices),
        config.settings,
        core::defaultEnvironment(),
        SessionHostConfig {
            .pumping = SessionPumping::Threaded,
            .sizePolicy = config.sizePolicy,
            .defaultCellPixelSize = config.defaultCellPixelSize,
            .startupLayout = config.startupLayout,
            .identity = thisHostIdentity(),
            .workspaceGit = WorkspaceGitServices { .probe = *gitProbe, .executor = gitExecutor },
            .extensions = config.extensions ? config.extensions() : nullptr,
            .profiles = config.profiles,
            .profilePtys = config.profiles
                               ? makeProfilePtyFactory(config.shell, config.socketPath, shellDevices)
                               : ProfilePtyFactory {},
            .instance = lineage.instance,
            .restore = std::move(restore),
            .handover = std::exchange(image, std::nullopt),
            .adoptChild = daemonKeeper.childAdopter(),
            .seedRecovered = std::move(recovery.seeder) }
    };
    // Destroyed before the host: an end the daemon did not announce (an exception during the setup, every
    // listener gone on its own) leaves the sessions to the keeper, not their Releases.
    auto const sealed = daemonKeeper.sealAtExit();
    // The host restored or recovered every session, the longest part of a start: a milestone.
    daemonKeeper.beat();
    // What the keeper kept that no session took is let go.
    daemonKeeper.releaseUnadopted();
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
        persistence.emplace(
            sequence, loop, host, *store, restoredFrom, config.persistence, wallClock, recovery.pendingCopy);
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
    // Built before the runtime, which states its fingerprint, and before the listener is taken, so that a
    // listener it cannot encrypt is closed, unserved.
    auto tls = nativeTcpTlsOf(config.nativeTcp,
                              inherited,
                              core::platform::NativeFileSystem::instance(),
                              NativeFilePrivacy::instance());
    auto runtime = DaemonRuntime { runtimeConfigOf(config,
                                                   lineage.upgradeGeneration,
                                                   tls ? tls->fingerprint : std::string {},
                                                   daemonKeeper.processId()),
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
    // The socket file of each endpoint served on one, which a keeper removes once no daemon serves it.
    auto socketPaths = SocketPaths {};
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
        socketPaths.insert_or_assign(std::string { name }, path);
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
        socketPaths.insert_or_assign("control", core::pathToUtf8(config.socketPath));
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
        auto const compatPath = tmuxCompatPathOf(*config.tmuxCompatLabel);
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
        // The handler first: a listener it refuses to serve (no credentials) is never bound.
        auto handler = nativeTcpHandler(
            loop,
            host,
            tls,
            daemonNativeServices(runtime, controlService, config, acceptedTokensOf(*config.nativeTcp)),
            *config.nativeTcp);
        auto listened = std::optional<std::unique_ptr<core::net::IListener>> {};
        if (handler)
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
        if (listened)
        {
            auto const boundPort = (*listened)->boundPort();
            nativeTcpServer.emplace(loop, "native-tcp", std::move(*listened), std::move(*handler));
            servers.push_back(&*nativeTcpServer);
            if (tls->generated)
                tlsIdentities.insert_or_assign("native-tcp", *std::move(tls->generated));
            daemonLog()("native TCP listener on {}:{}, TLS fingerprint {}",
                        config.nativeTcp->host,
                        boundPort,
                        tls->fingerprint);
        }
        else
            unserved("native-tcp");
    }

    inherited.closeUnclaimed();

#ifndef _WIN32
    // The identities the daemon made up travel in the structure its keeper keeps, so that a restart after a
    // crash presents the very same certificate.
    auto structureExtras = keptIdentitiesOf(daemonKeeper.structureSink(), tlsIdentities);
#endif

    // Every listener is bound: each crosses an upgrade under its name, with the identity the daemon made up
    // for it. Kept here only by an upgrade, which hands it on; beside that, only the keeper's structure keeps
    // it (StructureExtras, and the keeper's copy, which no other process can read: the keeper is not
    // dumpable).
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
            liveUpgradeConfigOf(
                config, *config.liveUpgrade, lineage, std::move(tlsIdentities), daemonKeeper.crossing()));

#ifdef _WIN32
    daemonLog()("serving on {} (native: {})", core::pathToUtf8(config.socketPath), nativePath);
#else
    daemonLog()(
        "serving on {} (native: {}, tmux: {})", core::pathToUtf8(config.socketPath), nativePath, imsgPath);
#endif
    // Every listener is bound: the keeper keeps the structure, and keeps it current from now on, before any
    // client can see an id; then a duplicate of each listener, then hears that the daemon serves, and from
    // now on that it is alive.
#ifndef _WIN32
    auto structure = std::optional<keeperlink::StructurePusher> {};
    if (auto* const sink = daemonKeeper.structureSink())
        structure.emplace(loop,
                          host,
                          *sink,
                          keeperlink::StructureExtras { .daemonInstance = lineage.instance,
                                                        .release = config.releaseVersion,
                                                        .upgradeGeneration = lineage.upgradeGeneration,
                                                        .listeners = std::move(structureExtras) });
#endif
    daemonKeeper.startKeeping(config, servers, socketPaths);
    if (servers.empty())
    {
        // Only a daemon that took over hosted programs gets here: they run on, unreachable, until it is
        // asked to end.
        errorLog()("no endpoint could be served; the adopted programs run on until the daemon is stopped");
        daemonKeeper.serve(loop, awaitShutdown(&loop));
    }
    else
        daemonKeeper.serve(loop, serveAll(servers));
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
