// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points (`contour daemon` / `contour client`) — kept in
/// this Qt-free module so the whole serving path never touches the GUI stack.

#include <vtbackend/screen/Settings.hpp>

#include <vtpty/Process.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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

} // namespace vthost
