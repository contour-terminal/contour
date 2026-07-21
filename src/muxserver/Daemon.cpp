// SPDX-License-Identifier: Apache-2.0
#include <muxserver/Daemon.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <print>
#include <thread>
#include <utility>

#include <coro/WhenAll.hpp>
#include <muxserver/MuxServer.h>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/client/AttachClient.h>
#include <muxserver/client/TtyRenderer.h>
#include <muxserver/tmux/ControlSession.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>

#ifndef _WIN32
    #include <sys/ioctl.h>

    #include <array>
    #include <csignal>

    #include <termios.h>
    #include <unistd.h>
#endif

namespace muxserver
{

#ifndef _WIN32

namespace
{
    /// Drives both protocol servers' accept loops on the one reactor.
    coro::Task<void> serveBoth(MuxServer* control, MuxServer* native)
    {
        co_await coro::whenAll(control->serve(), native->serve());
    }
} // namespace

int runDaemon(DaemonConfig const& config)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    auto host = SessionHost {
        loop,
        [shell = config.shell](vtbackend::PageSize pageSize) -> std::unique_ptr<vtpty::Pty> {
            return std::make_unique<vtpty::Process>(
                shell, vtpty::createPty(pageSize, std::nullopt), /*escapeSandbox=*/true);
        },
        config.settings,
    };

    auto listener = net::listenUnix(loop, config.socketPath.string());
    if (!listener)
    {
        std::println(stderr, "contour daemon: {}", listener.error().toString());
        return EXIT_FAILURE;
    }

    auto server = MuxServer { loop, std::move(*listener), tmux::makeControlModeHandler(loop, host) };

    // The native cells+deltas protocol listens beside the control-mode socket.
    auto const nativePath = config.socketPath.string() + "-native";
    auto nativeListener = net::listenUnix(loop, nativePath);
    if (!nativeListener)
    {
        std::println(stderr, "contour daemon: {}", nativeListener.error().toString());
        return EXIT_FAILURE;
    }
    auto nativeServer = MuxServer { loop, std::move(*nativeListener), makeNativeHandler(loop, host) };

    // Signal handling without async-signal-safety hazards: SIGINT/SIGTERM are
    // blocked process-wide and consumed by a dedicated sigwait thread, which
    // marshals the shutdown onto the loop via post() (the loop's only
    // thread-safe entry point).
    auto signals = sigset_t {};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    auto signalSeen = std::atomic<bool> { false };
    auto watcher = std::thread { [&] {
        auto sig = 0;
        sigwait(&signals, &sig);
        signalSeen = true;
        loop.post([&] {
            server.close();
            nativeServer.close();
            loop.requestStop();
        });
    } };

    std::println(
        stderr, "contour daemon: serving on {} (native: {})", config.socketPath.string(), nativePath);
    loop.blockOn(serveBoth(&server, &nativeServer));

    // Unblock the watcher if shutdown came from elsewhere; if it already
    // consumed a signal, the raised one stays blocked and dies with the process.
    if (!signalSeen)
        std::ignore = ::raise(SIGTERM);
    watcher.join();

    std::println(stderr, "contour daemon: shut down");
    return EXIT_SUCCESS;
}

namespace
{
    /// Puts the controlling TTY into raw mode for the attach lifetime and
    /// restores the saved settings on destruction.
    class RawTty
    {
      public:
        RawTty()
        {
            if (::tcgetattr(STDIN_FILENO, &_saved) != 0)
                return;
            auto raw = _saved;
            ::cfmakeraw(&raw);
            _active = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }

        ~RawTty()
        {
            if (_active)
                ::tcsetattr(STDIN_FILENO, TCSANOW, &_saved);
        }

        RawTty(RawTty const&) = delete;
        RawTty& operator=(RawTty const&) = delete;
        RawTty(RawTty&&) = delete;
        RawTty& operator=(RawTty&&) = delete;

      private:
        termios _saved {};
        bool _active = false;
    };

    /// Forwards local keystrokes to the attached session; Ctrl-\ detaches.
    coro::Task<void> pumpStdin(net::EventLoop* loop,
                               client::AttachClient* attach,
                               std::uint64_t const* activeSession)
    {
        while (true)
        {
            co_await loop->waitReadable(STDIN_FILENO);
            auto buffer = std::array<char, 512> {};
            auto const n = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (n <= 0)
                break;
            auto const bytes = std::string_view { buffer.data(), static_cast<std::size_t>(n) };
            if (bytes.contains('\x1c')) // Ctrl-\ = detach
                break;
            if (*activeSession != 0)
                attach->sendInput(*activeSession, bytes);
        }
        attach->detach();
    }

    /// Proposes the local TTY's size once the handshake is on the wire.
    coro::Task<void> proposeLocalSize(client::AttachClient* attach)
    {
        auto size = winsize {};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0)
            attach->requestResize(size.ws_col, size.ws_row);
        co_return;
    }

    /// The whole attach lifetime: connect, mirror, pump input, detach.
    coro::Task<void> attachFlow(net::EventLoop* loop, std::string path, int* exitCode)
    {
        auto socket = co_await net::connectUnix(loop, path);
        if (!socket.has_value())
        {
            std::println(stderr, "contour attach: {}", socket.error().toString());
            *exitCode = EXIT_FAILURE;
            co_return;
        }

        auto attach = client::AttachClient { *loop, std::move(*socket) };
        auto activeSession = std::uint64_t { 0 };
        attach.setUpdateHandler([&activeSession](client::RemoteScreen const& screen) {
            activeSession = screen.session;
            auto const bytes = client::renderViewport(screen);
            std::ignore = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        });

        {
            auto const rawMode = RawTty {};
            // whenAll starts run() first, so its ClientHello precedes the
            // resize proposal in the write queue.
            co_await coro::whenAll(
                attach.run(), pumpStdin(loop, &attach, &activeSession), proposeLocalSize(&attach));
        }

        std::println("\ncontour attach: detached");
        if (attach.versionMismatch())
        {
            std::println(stderr, "contour attach: daemon speaks an incompatible protocol version");
            *exitCode = EXIT_FAILURE;
        }
    }
} // namespace

int runAttach(std::filesystem::path const& socketPath)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    auto exitCode = EXIT_SUCCESS;
    // The daemon's native protocol listens beside the control-mode socket.
    loop.blockOn(attachFlow(&loop, socketPath.string() + "-native", &exitCode));
    return exitCode;
}

#else // _WIN32

int runDaemon(DaemonConfig const& /*config*/)
{
    std::println(stderr, "contour daemon: not supported on Windows yet");
    return EXIT_FAILURE;
}

int runAttach(std::filesystem::path const& /*socketPath*/)
{
    std::println(stderr, "contour attach: not supported on Windows yet");
    return EXIT_FAILURE;
}

#endif // _WIN32

} // namespace muxserver
