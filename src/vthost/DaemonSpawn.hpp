// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Reaching a daemon and starting one on demand: the attach endpoints, the argv an auto-spawned daemon
/// gets, and the probe-and-spawn loop behind `ensureDaemon`. Split from Daemon.hpp, which holds what
/// *serving* needs, so a client that only attaches does not parse DaemonConfig and the settings it carries.

#include <vtpty/ProcessPlacement.hpp>

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ClientSizePolicy.hpp>
#include <vthost/DaemonLifecycle.hpp>

namespace vthost
{

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
