// SPDX-License-Identifier: Apache-2.0
#include <muxserver/Daemon.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
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
#include <muxserver/tmux/ImsgServer.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>

#ifndef _WIN32
    #include <sys/ioctl.h>

    #include <csignal>

    #include <termios.h>
    #include <unistd.h>
#else
// clang-format off
    #include <winsock2.h>
    #include <windows.h>
// clang-format on
#endif

namespace muxserver
{

#ifndef _WIN32

namespace
{
    /// Drives every protocol server's accept loop on the one reactor.
    coro::Task<void> serveAll(std::vector<MuxServer*> servers)
    {
        auto accepts = std::vector<coro::Task<void>> {};
        accepts.reserve(servers.size());
        for (auto* server: servers)
            accepts.push_back(server->serve());
        co_await coro::whenAll(std::move(accepts));
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

    // The imsg endpoint serves the REAL tmux client binary
    // (`tmux -S <socket>-tmux -C attach-session`).
    auto const imsgPath = config.socketPath.string() + "-tmux";
    auto imsgListener = net::listenUnix(loop, imsgPath);
    if (!imsgListener)
    {
        std::println(stderr, "contour daemon: {}", imsgListener.error().toString());
        return EXIT_FAILURE;
    }
    auto imsgServer = MuxServer { loop, std::move(*imsgListener), tmux::makeTmuxImsgHandler(loop, host) };

    auto servers = std::vector<MuxServer*> { &server, &nativeServer, &imsgServer };

    // Opt-in: ALSO bind tmux's own discovery path, so a plain
    // `tmux -L <label> -C attach-session` finds this daemon. Opt-in only —
    // with the daemon down, a `new-session` on that path silently forks a
    // REAL tmux server onto it.
    auto compatServer = std::optional<MuxServer> {};
    if (config.tmuxCompatLabel)
    {
        auto const compatPath = std::format("/tmp/tmux-{}/{}", ::getuid(), *config.tmuxCompatLabel);
        auto compatListener = net::listenUnix(loop, compatPath);
        if (!compatListener)
        {
            std::println(stderr, "contour daemon: {}", compatListener.error().toString());
            return EXIT_FAILURE;
        }
        compatServer.emplace(loop, std::move(*compatListener), tmux::makeTmuxImsgHandler(loop, host));
        servers.push_back(&*compatServer);
        std::println(stderr, "contour daemon: tmux-compat socket at {}", compatPath);
    }

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
            for (auto* each: servers)
                each->close();
            loop.requestStop();
        });
    } };

    std::println(stderr,
                 "contour daemon: serving on {} (native: {}, tmux: {})",
                 config.socketPath.string(),
                 nativePath,
                 imsgPath);
    loop.blockOn(serveAll(servers));

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
        attach.setUpdateHandler(
            [&activeSession](client::RemoteScreen const& screen, proto::Delta const& /*delta*/) {
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

namespace
{
    /// The console-control shutdown hook: SetConsoleCtrlHandler's callback is
    /// a plain C function invoked on an arbitrary thread with no user data
    /// pointer, so the running daemon registers its (thread-safe) shutdown
    /// poster in this one process-wide slot.
    std::atomic<std::function<void()>*> gConsoleShutdown = nullptr;

    BOOL WINAPI consoleCtrlHandler(DWORD type)
    {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
            return FALSE;
        if (auto* shutdown = gConsoleShutdown.load())
            (*shutdown)();
        return TRUE;
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

    auto const nativePath = config.socketPath.string() + "-native";
    auto nativeListener = net::listenUnix(loop, nativePath);
    if (!nativeListener)
    {
        std::println(stderr, "contour daemon: {}", nativeListener.error().toString());
        return EXIT_FAILURE;
    }
    auto nativeServer = MuxServer { loop, std::move(*nativeListener), makeNativeHandler(loop, host) };
    // No imsg endpoint on Windows: SCM_RIGHTS fd passing does not exist here.

    auto shutdown = std::function<void()> { [&] {
        loop.post([&] {
            server.close();
            nativeServer.close();
            loop.requestStop();
        });
    } };
    gConsoleShutdown = &shutdown;
    ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    std::println(
        stderr, "contour daemon: serving on {} (native: {})", config.socketPath.string(), nativePath);
    auto serveBoth = [](MuxServer* control, MuxServer* native) -> coro::Task<void> {
        co_await coro::whenAll(control->serve(), native->serve());
    };
    loop.blockOn(serveBoth(&server, &nativeServer));

    ::SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    gConsoleShutdown = nullptr;

    std::println(stderr, "contour daemon: shut down");
    return EXIT_SUCCESS;
}

namespace
{
    /// Puts the console into raw VT mode for the attach lifetime and restores
    /// the saved modes on destruction.
    class RawConsole
    {
      public:
        RawConsole(): _stdin(::GetStdHandle(STD_INPUT_HANDLE)), _stdout(::GetStdHandle(STD_OUTPUT_HANDLE))
        {
            if (::GetConsoleMode(_stdin, &_savedInput))
            {
                auto const raw =
                    (_savedInput
                     & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))
                    | ENABLE_VIRTUAL_TERMINAL_INPUT;
                ::SetConsoleMode(_stdin, raw);
            }
            if (::GetConsoleMode(_stdout, &_savedOutput))
                ::SetConsoleMode(
                    _stdout, _savedOutput | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        }

        ~RawConsole()
        {
            ::SetConsoleMode(_stdin, _savedInput);
            ::SetConsoleMode(_stdout, _savedOutput);
        }

        RawConsole(RawConsole const&) = delete;
        RawConsole& operator=(RawConsole const&) = delete;
        RawConsole(RawConsole&&) = delete;
        RawConsole& operator=(RawConsole&&) = delete;

      private:
        HANDLE _stdin;
        HANDLE _stdout;
        DWORD _savedInput = 0;
        DWORD _savedOutput = 0;
    };

    /// Proposes the local console's cell size as the daemon's client area.
    coro::Task<void> proposeConsoleSize(client::AttachClient* attach)
    {
        auto info = CONSOLE_SCREEN_BUFFER_INFO {};
        if (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &info))
        {
            auto const columns = static_cast<uint32_t>(info.srWindow.Right - info.srWindow.Left + 1);
            auto const lines = static_cast<uint32_t>(info.srWindow.Bottom - info.srWindow.Top + 1);
            if (columns > 0 && lines > 0)
                attach->requestResize(columns, lines);
        }
        co_return;
    }

    /// The attach flow on Windows: mirrors POSIX attachFlow, but console
    /// input cannot park on the socket reactor — a dedicated blocking-read
    /// thread posts the bytes onto the loop instead.
    coro::Task<int> attachFlowWin32(net::EventLoop* loop, std::filesystem::path path)
    {
        auto socket = co_await net::connectUnix(loop, path.string());
        if (!socket)
        {
            std::println(stderr, "contour attach: {}", socket.error().toString());
            co_return EXIT_FAILURE;
        }

        auto attach = client::AttachClient { *loop, std::move(*socket) };
        auto activeSession = std::uint64_t { 0 };
        attach.setUpdateHandler(
            [&activeSession](client::RemoteScreen const& screen, proto::Delta const& /*delta*/) {
                activeSession = screen.session;
                auto const bytes = client::renderViewport(screen);
                auto written = DWORD { 0 };
                ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE),
                            bytes.data(),
                            static_cast<DWORD>(bytes.size()),
                            &written,
                            nullptr);
            });

        auto const rawMode = RawConsole {};
        auto detached = std::atomic<bool> { false };

        // Console reads block; ReadFile on the input handle cannot join the
        // socket reactor. The reader thread is detached deliberately: it may
        // sit in a blocking ReadFile at process exit, which teardown reaps.
        auto reader = std::thread { [loop, &attach, &activeSession, &detached] {
            auto buffer = std::array<char, 512> {};
            while (!detached.load())
            {
                auto read = DWORD { 0 };
                if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE),
                                buffer.data(),
                                static_cast<DWORD>(buffer.size()),
                                &read,
                                nullptr)
                    || read == 0)
                    break;
                auto bytes = std::string { buffer.data(), read };
                if (bytes.contains('\x1c')) // Ctrl-\ detaches
                {
                    detached = true;
                    loop->post([&attach] { attach.detach(); });
                    break;
                }
                loop->post([&attach, &activeSession, moved = std::move(bytes)] {
                    if (activeSession != 0)
                        attach.sendInput(activeSession, moved);
                });
            }
        } };
        reader.detach();

        co_await coro::whenAll(attach.run(), proposeConsoleSize(&attach));
        detached = true;

        std::println(stderr, "\ncontour attach: detached");
        co_return attach.versionMismatch() ? EXIT_FAILURE : EXIT_SUCCESS;
    }
} // namespace

int runAttach(std::filesystem::path const& socketPath)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto nativePath = socketPath;
    nativePath += "-native";
    return loop.blockOn(attachFlowWin32(&loop, nativePath));
}

#endif // _WIN32

} // namespace muxserver
