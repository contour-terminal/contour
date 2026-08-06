// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points (`contour daemon` / `contour client`) — kept in
/// this Qt-free module so the whole serving path never touches the GUI stack.

#include <vtbackend/Settings.hpp>

#include <vtpty/Process.hpp>

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

#include <coro/Task.hpp>
#include <net/ISocket.hpp>
// Supplies DaemonConfig::settings' default, plus DefaultSessionHistoryLineCount and
// defaultSessionSettings(), which used to be DECLARED here. They moved down because SessionHost.h
// needs them too and sits BELOW this header — a lower layer must not reach up for them.
#include <vthost/ClientSizePolicy.hpp>
#include <vthost/SessionSettings.hpp>

namespace net
{
class EventLoop;
}

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
    std::string token;              ///< Preshared token required of every client.
    /// PEM certificate + private key files for TLS. When both are empty the
    /// daemon generates an ephemeral self-signed certificate (the TOFU default);
    /// the TCP transport is ALWAYS encrypted.
    std::string tlsCertPath;
    std::string tlsKeyPath;
};

/// Whether a daemon outlives its last hosted session.
enum class DaemonLifecycle : std::uint8_t
{
    /// Keeps serving with zero sessions — a user-started `contour daemon`, whose whole point is
    /// to still be there when the next client attaches.
    Persistent,
    /// Terminates once the last hosted session is gone — an auto-spawned daemon, which belongs
    /// to the client that started it (tmux spells this `exit-empty`).
    ExitWhenEmpty,
};

/// Everything `contour daemon` needs to serve.
struct DaemonConfig
{
    /// When set, ALSO serves the native protocol over TCP (opt-in; see the struct).
    std::optional<NativeTcpListenerConfig> nativeTcp;
    /// The control-socket file (see muxSocketPath for derivation).
    std::filesystem::path socketPath;
    /// Factory settings for every hosted session's terminal. Defaulted rather than left
    /// bare so a daemon is correct standalone; `contour daemon` overwrites it from the
    /// resolved profile, which `src/vthost` cannot reach on its own.
    vtbackend::Settings settings = defaultSessionSettings();
    /// The shell each new session runs.
    vtpty::Process::ExecInfo shell;
    /// When set, ALSO binds tmux's own discovery path
    /// `/tmp/tmux-<uid>/<label>` for the imsg endpoint, so a plain
    /// `tmux -L <label> -C attach-session` finds this daemon. Opt-in only.
    std::optional<std::string> tmuxCompatLabel;
    /// Whether this daemon outlives its last session. `Persistent` by default, so a user-started
    /// `contour daemon` keeps serving with none — that persistence is the whole point of starting
    /// one by hand. `ensureDaemon` starts the daemons IT spawns with `ExitWhenEmpty`, which is
    /// what `--exit-with-last-session` selects.
    DaemonLifecycle lifecycle = DaemonLifecycle::Persistent;
    /// How the authoritative client area is resolved when several attached clients report
    /// different ones. `Latest` by default, matching tmux's `window-size` — the window you just
    /// resized is the one you are looking at.
    ClientSizePolicy sizePolicy = ClientSizePolicy::Latest;
};

/// Runs the daemon: binds the hardened control socket, serves connections until
/// SIGINT/SIGTERM, then shuts down cleanly. Blocks the calling thread.
/// @param config The daemon configuration.
/// @return The process exit code (EXIT_SUCCESS on clean shutdown).
[[nodiscard]] int runDaemon(DaemonConfig const& config);

/// Starts @p commandLine as a detached daemon and blocks until it is serving.
///
/// What `contour daemon --background` runs. The caller supplies the whole argv rather than
/// options to rebuild one from: the CLI surface lives in `src/contour`, and the backgrounding
/// daemon relaunches ITSELF, so replaying its own tokens is both exact and immune to a new
/// option being forgotten here (see crispy::app::commandLine).
///
/// Returning means the daemon ACCEPTS — not merely that the process started. A `--background`
/// that returned earlier would hand the shell back before a bind failure could surface, which
/// is indistinguishable to the user from a daemon that started and immediately died.
/// @param commandLine The full argv for the daemon process (argv[0] first); must NOT itself
///        carry `--background`, or the child would background in turn and never bind.
/// @param socketPath The control socket the spawned daemon binds; readiness is probed on the
///        native endpoint beside it.
/// @param timeout How long to wait for it to accept connections.
/// @return EXIT_SUCCESS once it is serving, EXIT_FAILURE on spawn failure or timeout.
[[nodiscard]] int runDaemonDetached(std::vector<std::string> commandLine,
                                    std::filesystem::path const& socketPath,
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

/// Splits a `HOST:PORT` (or `[v6]:PORT`) string into its parts.
/// @param spec The endpoint string; the port must be a decimal in [1, 65535].
/// @return The host and port, or nullopt if @p spec is malformed.
[[nodiscard]] std::optional<std::pair<std::string, std::uint16_t>> parseHostPort(std::string_view spec);

/// The preshared token an endpoint carries (empty for the unix socket, whose
/// filesystem permissions are the gate; the ClientHello sends it verbatim).
[[nodiscard]] std::string endpointToken(AttachEndpoint const& endpoint);

/// Connects to the daemon per @p endpoint: the local unix control socket (its
/// native socket resolved beside it), or a TLS-encrypted TCP connection whose
/// peer trust follows the endpoint's @c caPem (empty ⇒ TOFU). This establishes —
/// and for TCP encrypts — the transport; the caller's NativeClient sends the
/// token. Shared by the GUI NativeController.
/// @param loop The event loop whose reactor drives the connect (not owned).
/// @param endpoint The daemon endpoint.
/// @return The ready transport, or a human-readable error string.
[[nodiscard]] coro::Task<std::expected<std::unique_ptr<net::ISocket>, std::string>> connectAttach(
    net::EventLoop* loop, AttachEndpoint endpoint);

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

/// The argv an auto-spawned daemon is started with.
///
/// ONE spelling of the flag list: POSIX `execv` wants an argv array and Win32 `CreateProcess` a
/// command line, and while the list was spelled twice, a flag added to one platform was silently
/// missing on the other.
/// @param daemonBinary argv[0] — the contour binary to exec.
/// @param socketPath The control socket the spawned daemon binds.
/// @param spawnOptions What to forward to the spawned daemon; empty entries are omitted.
/// @param lifecycle Whether the spawned daemon ends with its last session.
/// @return The argv, argv[0] first.
[[nodiscard]] std::vector<std::string> daemonSpawnArgs(std::string_view daemonBinary,
                                                       std::filesystem::path const& socketPath,
                                                       DaemonSpawnOptions const& spawnOptions,
                                                       DaemonLifecycle lifecycle);

/// The character separating entries in the PATH environment variable on this platform.
inline constexpr char PathListSeparator =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

/// Resolves @p program to a path `execv` can actually run.
///
/// `execv` does NOT search PATH — and a client started as plain `contour` has exactly `contour` as
/// its argv[0], which is what the spawn inherits. Exec'ing it verbatim then fails with ENOENT, the
/// child exits, and the parent sits out its whole readiness timeout before reporting that the
/// daemon did not start: auto-spawn was broken for every invocation not spelled out as a path.
///
/// A name that carries a directory is already a path and passes through untouched (execvp's own
/// rule, so `./contour` and `/usr/bin/contour` keep meaning what they say). Anything else is looked
/// up in @p searchPath left-to-right, and the first entry for which @p isExecutable holds wins.
/// The predicate is injected so the search is testable without touching a filesystem — and because
/// "may this process execute it" is a platform question the search itself has no business knowing.
///
/// Resolution happens in the PARENT, before fork: between fork and exec only async-signal-safe
/// calls are allowed, and a PATH walk allocates.
/// @param program The binary as named by the caller (typically argv[0]).
/// @param searchPath The PATH value to search, @ref PathListSeparator separated.
/// @param isExecutable Whether a candidate path names something this process may execute.
/// @return The resolved path, or @p program unchanged when nothing matched.
[[nodiscard]] std::string resolveExecutablePath(
    std::string_view program,
    std::string_view searchPath,
    std::function<bool(std::filesystem::path const&)> const& isExecutable);

/// Joins @p args into a Win32 command line, quoting each argument whole so a path containing
/// spaces survives CommandLineToArgv — including argv[0], which the hand-rolled formatting this
/// replaced left bare (breaking `C:\Program Files\...\contour.exe`).
///
/// Quoting is the full `CommandLineToArgvW` rule, not a pair of surrounding quotes: a backslash run
/// immediately before a `"` is halved and the quote it precedes is consumed, so `--config=C:\dir\`
/// naively wrapped ends its own quote and swallows the rest of the command line into that one
/// argument. Backslash runs before the closing quote are therefore doubled and embedded quotes
/// escaped, exactly as Microsoft's parser undoes.
///
/// Compiled on every platform, not only Windows, so the quoting is covered by the Linux CI run
/// rather than solely by a Windows build.
///
/// NOTE: `vtpty::Process::start()` (Process_win32.cpp) builds its own command line with weaker
/// quoting — it quotes only arguments containing a space and never argv[0]. The two are NOT unified
/// because that one spawns arbitrary shells, where quoting every argument could change how a
/// program parses its own arguments; unifying them needs a real Windows build to verify.
/// @param args The argv to join.
/// @return The command line.
[[nodiscard]] std::string joinCommandLine(std::span<std::string const> args);

/// Ensures a daemon is running at @p endpoint. For Unix sockets, spawns
/// `contour daemon` in the background if the socket isn't already accepting
/// connections, then blocks until the daemon is ready or @p timeout elapses.
/// For TCP endpoints this is a no-op (remote daemons can't be auto-spawned).
///
/// A daemon spawned here is started `--exit-with-last-session` and therefore ends with its last
/// session: it belongs to the client that needed it, and "make sure one exists" does not mean
/// "leave one behind". Note this is a property of the DAEMON, not of the spawning client — a
/// second client attaching inherits it, and the daemon ends when every session across every
/// client is gone. An already-running daemon keeps whatever policy it was started with.
/// @param endpoint Where to reach the daemon.
/// @param daemonBinary Path to the `contour` binary (argv[0] of the client).
/// @param spawnOptions Options forwarded to a spawned daemon's command line.
/// @param timeout How long to wait for a spawned daemon to accept connections.
/// @return EXIT_SUCCESS if a daemon is confirmed running, EXIT_FAILURE otherwise.
[[nodiscard]] int ensureDaemon(AttachEndpoint const& endpoint,
                               std::string_view daemonBinary,
                               DaemonSpawnOptions const& spawnOptions = {},
                               std::chrono::seconds timeout = std::chrono::seconds(5));

} // namespace vthost
