// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points behind `contour daemon` / `contour attach` — kept in
/// this Qt-free module so the whole serving path never touches the GUI stack.

#include <vtbackend/Settings.h>

#include <vtpty/Process.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#ifndef _WIN32
    #include <csignal>
    #include <functional>

    #include <coro/Task.hpp>
    #include <net/platform/NativeHandle.h>

namespace net
{
class EventLoop;
}
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

/// Attaches this terminal to a running daemon over the native cells+deltas
/// protocol: mirrors the remote screen onto the local TTY and forwards
/// keystrokes until the peer disconnects or Ctrl-\ detaches.
/// @param socketPath The daemon's control-socket file (the native socket
///        lives beside it).
/// @return The process exit code (EXIT_SUCCESS on clean detach).
[[nodiscard]] int runAttach(std::filesystem::path const& socketPath);

#ifndef _WIN32

/// RAII bridge turning SIGWINCH into an event-loop-waitable readiness signal.
///
/// On construction it creates a non-blocking, close-on-exec pipe and installs a
/// SIGWINCH handler whose only action is an async-signal-safe one-byte @c write
/// to the pipe's write end. A coroutine (see @c trackTtySize) awaits readability
/// on @c readFd(), drains the coalesced bytes, and re-proposes the local TTY
/// size. On destruction it restores the previous SIGWINCH disposition and closes
/// the pipe. At most one instance may exist at a time: it owns a process-global
/// handler slot the signal handler reads.
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
