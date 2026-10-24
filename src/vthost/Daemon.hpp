// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points (`contour daemon` / `contour client`) — kept in
/// this Qt-free module so the whole serving path never touches the GUI stack.

#include <vtbackend/screen/Settings.hpp>

#include <vtpty/Process.hpp>

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/AccessFile.hpp>
#include <vtworkspace/LayoutTree.hpp>
// Supplies DaemonConfig::settings' default, plus DefaultSessionHistoryLineCount and
// defaultSessionSettings(), which used to be DECLARED here. They moved down because SessionHost.h
// needs them too and sits BELOW this header — a lower layer must not reach up for them.
#include <vthost/ClientSizePolicy.hpp>
#include <vthost/ConfigReload.hpp>
#include <vthost/DaemonLifecycle.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/ProfileCatalog.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/ShutdownSignals.hpp>
#include <vthost/WorkspacePersistenceConfig.hpp>

namespace vthost
{

/// Opt-in TCP listener for the native cells+deltas protocol. Absent unless the
/// user configures it; when present it binds @c host (loopback by default) and
/// requires @c token of every client — the TCP transport has no filesystem gate,
/// so a non-empty token is the authentication. (TLS cert/key land with the TLS
/// decorator; until then a non-loopback bind must be tunnelled, e.g. over SSH.)
struct NativeTcpListenerConfig
{
    std::string host = "127.0.0.1"; ///< Bind address; loopback by default.
    std::uint16_t port = 0;         ///< TCP port (0 = OS-assigned ephemeral).
    std::string token;              ///< Preshared token required of every client; empty for none.
    /// Further preshared tokens, from `--access-file`, each with the control API grants it carries; a
    /// client presents @ref token or one of these. @see acceptedTokensOf
    std::vector<AcceptedToken> accessTokens {};
    /// PEM certificate + private key files for TLS. When both are empty the
    /// daemon generates an ephemeral self-signed certificate (the TOFU default);
    /// the TCP transport is ALWAYS encrypted.
    std::string tlsCertPath;
    std::string tlsKeyPath;
};

/// @param listener The TCP listener's configuration.
/// @return The tokens it accepts: its single token first, granting everything, when there is one, then the
///         access file's in file order. A token in both would grant everything, as the first match wins,
///         which is why parseAccessFile() refuses a line repeating the single token.
[[nodiscard]] std::vector<AcceptedToken> acceptedTokensOf(NativeTcpListenerConfig const& listener);

/// Everything `contour daemon` needs to serve.
struct DaemonConfig
{
    /// Which run of the daemon this is, as every ServerHello states it (proto::ServerHello::daemonInstance).
    /// Supplied by the composition root -- drawDaemonInstance() once per daemon process -- rather than drawn
    /// here, so a test states a fixed one; 0 states none. A configuration reload keeps it: the run goes on.
    uint64_t instance = 0;
    /// When set, ALSO serves the native protocol over TCP (opt-in; see the struct).
    std::optional<NativeTcpListenerConfig> nativeTcp;
    /// The control-socket file (see muxSocketPath for derivation).
    std::filesystem::path socketPath;
    /// Factory settings for every hosted session's terminal. Defaulted rather than left
    /// bare so a daemon is correct standalone; `contour daemon` overwrites it from the
    /// resolved profile, which `src/vthost` cannot reach on its own.
    vtbackend::Settings settings = defaultSessionSettings();
    /// The shell each new session runs, unless it runs under one of @ref profiles.
    vtpty::Process::ExecInfo shell;
    /// The host's profiles, by name: what a session a client names a profile for runs (settings and
    /// shell). Null for none — the bare `DaemonConfig{}` of a test — which keeps @ref settings and
    /// @ref shell for every session and offers no Capability::ProfileByName.
    std::shared_ptr<ProfileCatalog const> profiles {};
    /// Reloads @ref profiles while the daemon runs: polled on its loop, every catalog it yields replaces the
    /// host's (@see SessionHost::replaceProfiles). Unset for none, as a test's bare `DaemonConfig{}` has.
    std::optional<ConfigReloadHook> configReload {};
    /// Whether a spawned shell escapes its sandbox (Flatpak). `true` by default, matching a local
    /// session's profile default (`escape_sandbox: true`); `contour daemon` overwrites it from the
    /// resolved profile, same as `settings` and `shell` above.
    bool escapeSandbox = true;
    /// Moves each hosted shell into its own resource domain, so an out-of-memory kill inside one
    /// cannot stop the daemon's unit. NoPlacement by default, so a bare `DaemonConfig{}` -- a test --
    /// creates no systemd units; `contour daemon` sets the platform's default.
    std::shared_ptr<vtpty::ProcessPlacement> placement = std::make_shared<vtpty::NoPlacement>();
    /// The profile's configured startup layout (`default_layout` + `layouts:`), realized by
    /// `SessionHost` before any client attaches. Empty `tabs` (the default) means no layout is
    /// configured — `contour daemon` overwrites it from the resolved profile, same as `settings`,
    /// `shell` and `escapeSandbox` above; a bare `DaemonConfig{}` keeps today's single-default-tab
    /// behavior.
    vtworkspace::Layout startupLayout {};
    /// When set, ALSO binds tmux's own discovery path
    /// `/tmp/tmux-<uid>/<label>` for the imsg endpoint, so a plain
    /// `tmux -L <label> -C attach-session` finds this daemon. Opt-in only.
    std::optional<std::string> tmuxCompatLabel;
    /// Whether, and when, this daemon ends on its own. `Persistent` by default, so a user-started
    /// `contour daemon` keeps serving with no session — that persistence is the whole point of starting
    /// one by hand. `ensureDaemon` starts the daemons IT spawns with `ExitWhenIdle`; `--lifecycle`
    /// selects any of them, and `--exit-with-last-session` is the alias for `ExitWhenEmpty`.
    DaemonLifecycle lifecycle = DaemonLifecycle::Persistent;
    /// How the authoritative client area is resolved when several attached clients report
    /// different ones. `Latest` by default, matching tmux's `window-size` — the window you just
    /// resized is the one you are looking at.
    ClientSizePolicy sizePolicy = ClientSizePolicy::Latest;
    /// Makes the status-line extensions every hosted session runs; unset for none, which constructs, polls
    /// and wakes nothing of them. A factory rather than the extensions themselves, because they post to the
    /// daemon's event loop: runDaemon() makes them once that exists, and its session host destroys them
    /// before it goes. `contour daemon` sets it from the profile, which `src/vthost` cannot reach.
    std::function<std::shared_ptr<IExtensionHosting>()> extensions {};
    /// The release this daemon states in every ServerHello and status reply; empty when unknown.
    std::string releaseVersion {};
    /// The image this daemon runs from, as its status reply states it.
    std::filesystem::path executablePath {};
    /// The file this daemon logs to, as its status reply states it; empty for stderr.
    std::filesystem::path logFile {};
    /// How long an ExitWhenIdle daemon waits with no session and no client before it ends.
    std::chrono::seconds idleExitGrace = DefaultIdleExitGrace;
    /// Called once, on the loop thread, with what asked for the shutdown, before any listener closes and
    /// while every session is still alive. Unset for nothing; the seam a later phase hands sessions over
    /// through.
    std::function<void(ShutdownCause)> beforeShutdown {};
    /// Where the daemon hears that its process is asked to end. The process's own signals by default, so
    /// a daemon is correct standalone; a test running the daemon in-process sets NoShutdownSignals, whose
    /// watch neither takes the test process's signals nor sends one into it.
    /// Never null.
    std::shared_ptr<ShutdownSignals> shutdownSignals = processShutdownSignals();
    /// How the daemon keeps its workspace across a restart: restored at start from the snapshot file,
    /// written while it serves and as it ends (workspaceFor). An empty `snapshotPath` (the default, a test's
    /// bare `DaemonConfig{}`) keeps nothing and offers no Capability::WorkspaceRestore; `contour daemon`
    /// sets it from `daemon.restore`.
    WorkspacePersistenceConfig persistence {};
};

/// Creates the directory @p socketPath lives in owner-only, or checks that it is safe, as the
/// daemon's listener does when it binds there.
///
/// What runs before anything else could create that directory with looser permissions: the default
/// log file (daemonLogPath()) lies in it, and a log sink creates its parent with the umask's mode,
/// 0755 under the usual 022, which the listener then refuses as world-accessible. POSIX only; a no-op
/// on Windows, and for a socket named in the current directory, whose directory is not ours to change.
/// @param socketPath The daemon's control socket.
/// @return Nothing, or why the directory is unusable.
[[nodiscard]] std::expected<void, std::string> prepareSocketDirectory(
    std::filesystem::path const& socketPath);

/// Runs the daemon: binds the hardened control socket, serves connections until a shutdown is asked
/// for (see ShutdownCause), then shuts down cleanly. Blocks the calling thread.
/// @param config The daemon configuration.
/// @return The process exit code (EXIT_SUCCESS on clean shutdown).
[[nodiscard]] int runDaemon(DaemonConfig const& config);

/// Starts @p commandLine as a detached daemon and blocks until it is serving.
///
/// What `contour daemon --background` runs. The caller supplies the whole argv rather than
/// options to rebuild one from: the CLI surface lives in `src/contour`, and the backgrounding
/// daemon relaunches ITSELF, so replaying its own tokens is both exact and immune to a new
/// option being forgotten here (see core::cli::App::commandLine).
///
/// Returning means the daemon ACCEPTS — not merely that the process started. A `--background`
/// that returned earlier would hand the shell back before a bind failure could surface, which
/// is indistinguishable to the user from a daemon that started and immediately died.
/// @param commandLine The full argv for the daemon process (argv[0] first); must NOT itself
///        carry `--background`, or the child would background in turn and never bind.
/// @param socketPath The control socket the spawned daemon binds; readiness is probed on the
///        native endpoint beside it.
/// @param placement Places the daemon before it runs, as a session's shell is placed, so that it
///        lives in no tab's resource domain. POSIX only.
/// @param timeout How long to wait for it to accept connections.
/// @return EXIT_SUCCESS once it is serving, EXIT_FAILURE on spawn failure or timeout.
[[nodiscard]] int runDaemonDetached(std::vector<std::string> commandLine,
                                    std::filesystem::path const& socketPath,
                                    vtpty::ProcessPlacement& placement,
                                    std::chrono::seconds timeout = std::chrono::seconds(5));

/// Reaching the daemon over its local AF_UNIX control socket (the default).
struct UnixEndpoint
{
    /// The daemon's control-socket file; the native socket lives beside it.
    std::filesystem::path socketPath;
};

/// Reaching the daemon over TCP — ALWAYS TLS-encrypted, with @c token as the
/// authentication (the TCP transport has no filesystem gate).
struct TcpEndpoint
{
    std::string host;       ///< Remote host ("127.0.0.1", a hostname).
    std::uint16_t port = 0; ///< Remote port.
    std::string token;      ///< Preshared token sent in the ClientHello.
    /// Trust-anchor certificate (PEM) pinning the daemon's TLS cert. Empty ⇒ the
    /// TOFU posture (encrypt but do not verify the peer; the token authenticates).
    std::string caPem;
};

/// Where `contour client` reaches the daemon: the local unix socket or TCP+TLS.
using AttachEndpoint = std::variant<UnixEndpoint, TcpEndpoint>;

/// Draws which run of the daemon a new daemon process is (DaemonConfig::instance): at random, never 0. For
/// the composition root that starts a daemon, once per process.
/// @return The instance.
[[nodiscard]] uint64_t drawDaemonInstance();

/// Splits a `HOST:PORT` (or `[v6]:PORT`) string into its parts.
/// @param spec The endpoint string; the port must be a decimal in [1, 65535].
/// @return The host and port, or nullopt if @p spec is malformed.
[[nodiscard]] std::optional<std::pair<std::string, std::uint16_t>> parseHostPort(std::string_view spec);

/// What an auto-spawned daemon inherits from the client that spawned it.
///
/// Without these the auto-spawned daemon — which is the COMMON case, since the GUI starts one
/// on demand — is the one instance nobody can configure and whose stderr is the client's
/// inherited terminal. Passing them through is what makes it observable, and what keeps its
/// hosted terminals emulating the way the client's own configuration says they should.
/// Every field default-initialized so a caller naming a subset stays clean under
/// -Wmissing-designated-field-initializers.
struct DaemonSpawnOptions
{
    std::string filter {};  ///< The --log tag filter; empty passes no --log.
    std::string logFile {}; ///< The --log-file path; empty passes no --log-file.
    /// The --config file the daemon reads its sessions' terminal settings from; empty passes
    /// no --config, leaving the daemon on the default location.
    std::string configPath {};
    /// The --profile those settings come from; empty passes no --profile.
    std::string profileName {};
    /// How the daemon resolves one client area when several clients attach.
    ///
    /// Forwarded for the reason this whole struct exists: a client that auto-spawns its daemon
    /// would otherwise get `latest` no matter what it was asked for, and the auto-spawned daemon
    /// is the common case. Unlike the strings above there is no "unset" to test — absence IS a
    /// value — so this one is always passed, which also makes the daemon's `ps` line say which
    /// policy it is running.
    ClientSizePolicy sizePolicy = ClientSizePolicy::Latest;
};

/// The option daemonSpawnArgs spells the daemon's socket with, its value following directly. Shared with
/// registeredSocketPath(), which reads it back.
inline constexpr std::string_view SocketOption = "--socket=";

/// The argv an auto-spawned daemon is started with.
///
/// ONE spelling of the flag list: POSIX `execv` wants an argv array and Win32 `CreateProcess` a
/// command line, and while the list was spelled twice, a flag added to one platform was silently
/// missing on the other.
/// @param daemonBinary argv[0] — the contour binary to exec.
/// @param socketPath The control socket the spawned daemon binds.
/// @param spawnOptions What to forward to the spawned daemon; empty entries are omitted.
/// @param lifecycle When the spawned daemon ends on its own; spelled `--lifecycle=<name>`.
/// @return The argv, argv[0] first. It always carries `--log-file`: the given one, else
///         daemonLogPath(@p socketPath), so a spawned daemon's diagnostics are never lost.
[[nodiscard]] std::vector<std::string> daemonSpawnArgs(std::string_view daemonBinary,
                                                       std::filesystem::path const& socketPath,
                                                       DaemonSpawnOptions const& spawnOptions,
                                                       DaemonLifecycle lifecycle);

/// Reads the socket a registered daemon serves on back out of the command line its service backend
/// reports for the registration (ServiceStatus::commandLine), which daemonSpawnArgs spelled.
///
/// That text is one string, joined by the backend: each argument double-quoted (systemd, and the Task
/// Scheduler for an argument with a blank in it) or bare (launchd). A bare value ends at the next blank
/// and is taken as it stands. A quoted value ends at the first unescaped quote, with the escapes
/// systemdQuote() writes undone: `\\` and `\"`, and `%%` and `$$`; any other backslash is itself. That
/// also reads the Task Scheduler's quoting, which doubles a backslash only before the closing quote,
/// for every path but one holding a doubled backslash, `%` or `$` (a UNC `\\server` prefix, say).
/// @param commandLine The registration's command line.
/// @return The path after SocketOption; nullopt when the registration names none, and the daemon then
///         serves on its label's default socket.
[[nodiscard]] std::optional<std::filesystem::path> registeredSocketPath(std::string_view commandLine);

/// Joins @p args into the Win32 command line that runs them, args[0] the program: by
/// vtpty::buildWindowsCommandLine(), as a spawned shell's command line is spelled.
/// @param args The argv to join.
/// @return The command line; empty for no @p args.
[[nodiscard]] std::string joinCommandLine(std::span<std::string const> args);

class DaemonBinarySource;

/// Whether a daemon accepts connections.
enum class DaemonPresence : uint8_t
{
    Absent = 0,
    Serving,
};

class DaemonSpawner;

/// The sleep before the next readiness probe: a spawned daemon usually binds within a few
/// milliseconds, so the poll starts short and backs off by doubling.
/// @param previous The sleep just taken, or nullopt before the first one.
/// @return 5 ms for nullopt, otherwise twice @p previous, capped at one second.
[[nodiscard]] constexpr core::platform::SteadyDuration nextProbeDelay(
    std::optional<core::platform::SteadyDuration> previous) noexcept
{
    constexpr auto First = std::chrono::milliseconds { 5 };
    constexpr auto Cap = std::chrono::seconds { 1 };
    if (!previous)
        return First;
    return std::min<core::platform::SteadyDuration>(*previous * 2, Cap);
}

/// Waits for a daemon to accept connections.
/// @param spawner Asks whether a daemon serves.
/// @param socketPath The control socket the daemon binds.
/// @param timeout How long to wait before giving up.
/// @return Serving once it accepts; Absent when @p timeout elapsed first.
[[nodiscard]] DaemonPresence awaitDaemon(DaemonSpawner& spawner,
                                         std::filesystem::path const& socketPath,
                                         std::chrono::milliseconds timeout);

/// Starts a daemon process, and asks whether one serves: the two operating-system effects of
/// ensureDaemon(), behind an interface so the window between them is testable.
class DaemonSpawner
{
  public:
    virtual ~DaemonSpawner() = default;

    /// Starts @p argv detached from this process, so this process may exit without taking the daemon
    /// with it.
    /// @param argv The full argv; argv[0] is the binary to execute, looked up on PATH if bare.
    /// @param placement Places the daemon before it runs, as a session's shell is placed. POSIX only.
    /// @return Nothing once started, or why it could not be.
    [[nodiscard]] virtual std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                                 vtpty::ProcessPlacement& placement) = 0;

    /// @param socketPath A daemon's control socket; the native endpoint beside it is what is asked.
    /// @return Whether a daemon accepts connections there.
    [[nodiscard]] virtual DaemonPresence probe(std::filesystem::path const& socketPath) = 0;
};

/// @return The operating system's spawner: a detached process, and a connect to its native socket.
[[nodiscard]] std::unique_ptr<DaemonSpawner> makeNativeDaemonSpawner();

/// Ensures a daemon is running at @p endpoint. For Unix sockets, spawns
/// `contour daemon` in the background if the socket isn't already accepting
/// connections, then blocks until the daemon is ready or @p timeout elapses.
/// For TCP endpoints this is a no-op (remote daemons can't be auto-spawned).
///
/// The binary is asked for only once the probe found no daemon, since staging it can mean a copy of the
/// whole install. The image it hands out stays leased until the spawned daemon serves, or the spawn has
/// failed: by then the daemon holds its own lease, taken before it binds, so no prune of another
/// launcher can remove the image in between.
///
/// A daemon spawned here is started `--lifecycle=exit-when-idle` and therefore ends DefaultIdleExitGrace
/// after it has no session and no client: it belongs to the clients that needed it, and "make sure one
/// exists" does not mean "leave one behind". Note this is a property of the DAEMON, not of the spawning
/// client — a second client attaching inherits it. An already-running daemon keeps whatever policy it
/// was started with.
/// @param endpoint Where to reach the daemon.
/// @param binary What to start the daemon from: the `contour` binary, or a staged copy of it.
/// @param spawner What starts the daemon and asks whether it serves.
/// @param placement Places a spawned daemon before it runs, as a session's shell is placed: the
///        daemon holds every hosted session's scrollback, and must live in neither the client's own
///        resource domain nor the tab's it was started from. POSIX only.
/// @param spawnOptions Options forwarded to a spawned daemon's command line.
/// @param timeout How long to wait for a spawned daemon to accept connections.
/// @return EXIT_SUCCESS if a daemon is confirmed running, EXIT_FAILURE otherwise.
[[nodiscard]] int ensureDaemon(AttachEndpoint const& endpoint,
                               DaemonBinarySource& binary,
                               DaemonSpawner& spawner,
                               vtpty::ProcessPlacement& placement,
                               DaemonSpawnOptions const& spawnOptions,
                               std::chrono::seconds timeout);

/// ensureDaemon() through the operating system's spawner (makeNativeDaemonSpawner()).
/// @param endpoint Where to reach the daemon.
/// @param binary What to start the daemon from.
/// @param placement Places a spawned daemon before it runs. POSIX only.
/// @param spawnOptions Options forwarded to a spawned daemon's command line.
/// @param timeout How long to wait for a spawned daemon to accept connections.
/// @return EXIT_SUCCESS if a daemon is confirmed running, EXIT_FAILURE otherwise.
[[nodiscard]] int ensureDaemon(AttachEndpoint const& endpoint,
                               DaemonBinarySource& binary,
                               vtpty::ProcessPlacement& placement,
                               DaemonSpawnOptions const& spawnOptions = {},
                               std::chrono::seconds timeout = std::chrono::seconds(5));

} // namespace vthost
