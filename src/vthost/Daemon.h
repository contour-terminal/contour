// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points (`contour daemon` / `contour client`) — kept in
/// this Qt-free module so the whole serving path never touches the GUI stack.

#include <vtbackend/Settings.h>

#include <vtpty/Process.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <coro/Task.hpp>
#include <net/ISocket.h>

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

/// Everything `contour daemon` needs to serve.
struct DaemonConfig
{
    /// When set, ALSO serves the native protocol over TCP (opt-in; see the struct).
    std::optional<NativeTcpListenerConfig> nativeTcp;
    /// The control-socket file (see muxSocketPath for derivation).
    std::filesystem::path socketPath;
    /// Factory settings for every hosted session's terminal.
    vtbackend::Settings settings;
    /// The shell each new session runs.
    vtpty::Process::ExecInfo shell;
    /// When set, ALSO binds tmux's own discovery path
    /// `/tmp/tmux-<uid>/<label>` for the imsg endpoint, so a plain
    /// `tmux -L <label> -C attach-session` finds this daemon. Opt-in only.
    std::optional<std::string> tmuxCompatLabel;
};

/// Runs the daemon: binds the hardened control socket, serves connections until
/// SIGINT/SIGTERM, then shuts down cleanly. Blocks the calling thread.
/// @param config The daemon configuration.
/// @return The process exit code (EXIT_SUCCESS on clean shutdown).
[[nodiscard]] int runDaemon(DaemonConfig const& config);

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

/// Where `contour attach` reaches the daemon: the local unix socket or TCP+TLS.
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

/// Ensures a daemon is running at @p endpoint. For Unix sockets, spawns
/// `contour daemon` in the background if the socket isn't already accepting
/// connections, then blocks until the daemon is ready or @p timeout elapses.
/// For TCP endpoints this is a no-op (remote daemons can't be auto-spawned).
/// @param endpoint Where to reach the daemon.
/// @param daemonBinary Path to the `contour` binary (argv[0] of the client).
/// @param timeout How long to wait for a spawned daemon to accept connections.
/// @return EXIT_SUCCESS if a daemon is confirmed running, EXIT_FAILURE otherwise.
[[nodiscard]] int ensureDaemon(AttachEndpoint const& endpoint,
                               std::string_view daemonBinary,
                               std::chrono::seconds timeout = std::chrono::seconds(5));

} // namespace vthost
