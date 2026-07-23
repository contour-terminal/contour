// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points behind `contour daemon` / `contour attach` — kept in
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

#ifndef _WIN32
    #include <csignal>
    #include <functional>

    #include <net/platform/NativeHandle.h>
#endif

namespace muxserver
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
/// and for TCP encrypts — the transport; the caller's AttachClient sends the
/// token. Shared by the thin `contour attach` flow and the GUI AttachController.
/// @param loop The event loop whose reactor drives the connect (not owned).
/// @param endpoint The daemon endpoint.
/// @return The ready transport, or a human-readable error string.
[[nodiscard]] coro::Task<std::expected<std::unique_ptr<net::ISocket>, std::string>> connectAttach(
    net::EventLoop* loop, AttachEndpoint endpoint);

/// Attaches this terminal to a running daemon over the native cells+deltas
/// protocol: mirrors the remote screen onto the local TTY and forwards
/// keystrokes until the peer disconnects or Ctrl-\ detaches.
/// @param endpoint The daemon endpoint (unix socket or TCP+TLS).
/// @return The process exit code (EXIT_SUCCESS on clean detach).
[[nodiscard]] int runAttach(AttachEndpoint const& endpoint);

#ifndef _WIN32

/// RAII bridge turning SIGWINCH into an event-loop-waitable readiness signal.
///
/// On construction it creates a non-blocking, close-on-exec pipe and registers
/// its write end in a shared, lock-free registry. A SIGWINCH handler — installed
/// once, process-wide — writes a byte to every registered pipe in the registry.
/// A coroutine (see @c trackTtySize) awaits readability on @c readFd(), drains
/// the coalesced bytes, and re-proposes the local TTY size. On destruction it
/// unregisters the fd, restores the previous SIGWINCH disposition (when it was
/// the last instance), and closes the pipe. Multiple instances may coexist:
/// the shared registry makes the handler fan out to all live notifiers.
class SigwinchNotifier
{
  public:
    /// Creates the self-pipe and installs the SIGWINCH handler. On pipe-creation
    /// failure the object is left invalid (@c valid() is false) rather than
    /// throwing; the attach then runs without live resize propagation.
    SigwinchNotifier();

    /// Restores the previous SIGWINCH disposition and closes the pipe.
    ~SigwinchNotifier();

    SigwinchNotifier(SigwinchNotifier const&) = delete;
    SigwinchNotifier& operator=(SigwinchNotifier const&) = delete;
    SigwinchNotifier(SigwinchNotifier&&) = delete;
    SigwinchNotifier& operator=(SigwinchNotifier&&) = delete;

    /// @return The pipe's read end for the reactor to await, or @c net::InvalidHandle
    ///         when the pipe could not be created (SIGWINCH tracking disabled).
    [[nodiscard]] net::NativeHandle readFd() const noexcept { return _readFd; }

    /// @return True if the self-pipe and SIGWINCH handler are installed.
    [[nodiscard]] bool valid() const noexcept { return _readFd != net::InvalidHandle; }

  private:
    net::NativeHandle _readFd = net::InvalidHandle;
    net::NativeHandle _writeFd = net::InvalidHandle;
    int _slotIndex = -1; ///< Index in the shared gWinchWriteFds registry, or -1 if invalid.
    struct sigaction _previous {};
};

/// Proposes the local TTY size to the daemon once, then again on every SIGWINCH.
///
/// The first @p propose call is the initial proposal — because @c whenAny starts
/// @c AttachClient::run() (which enqueues the ClientHello) before this task, the
/// proposal is correctly ordered after the handshake. The coroutine then parks on
/// @p winchFd; each time a SIGWINCH byte arrives it drains the coalesced bytes and
/// calls @p propose again. It loops until cancelled by the winning sibling of the
/// enclosing @c whenAny. When @p winchFd is @c net::InvalidHandle (the self-pipe
/// could not be created) it stays alive but idle, so the attach still functions
/// without live resize propagation.
/// @param loop The event loop the flow runs on (a pointer — coroutine parameters
///        must not be references, they dangle when the caller's temporary dies).
/// @param winchFd The SIGWINCH self-pipe read end (see @c SigwinchNotifier::readFd).
/// @param propose Re-queries the local TTY size and proposes it to the daemon.
/// @return A task that never completes normally; it unwinds on cancellation.
[[nodiscard]] coro::Task<void> trackTtySize(net::EventLoop* loop,
                                            net::NativeHandle winchFd,
                                            std::function<void()> propose);

#endif // !_WIN32

} // namespace muxserver
