// SPDX-License-Identifier: Apache-2.0
#include <muxserver/Daemon.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <print>
#include <thread>
#include <utility>

#include <muxserver/MuxServer.h>
#include <muxserver/SessionHost.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>

#ifndef _WIN32
    #include <csignal>
#endif

namespace muxserver
{

#ifndef _WIN32

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

    auto server = MuxServer { loop, std::move(*listener), drainConnection };

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
            loop.requestStop();
        });
    } };

    std::println(stderr, "contour daemon: serving on {}", config.socketPath.string());
    loop.blockOn(server.serve());

    // Unblock the watcher if shutdown came from elsewhere; if it already
    // consumed a signal, the raised one stays blocked and dies with the process.
    if (!signalSeen)
        std::ignore = ::raise(SIGTERM);
    watcher.join();

    std::println(stderr, "contour daemon: shut down");
    return EXIT_SUCCESS;
}

int runAttachProbe(std::filesystem::path const& socketPath)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    auto probe = [](net::EventLoop* lp, std::string path, bool* connected) -> coro::Task<void> {
        auto socket = co_await net::connectUnix(lp, path);
        *connected = socket.has_value();
        if (!socket.has_value())
            std::println(stderr, "contour attach: {}", socket.error().toString());
    };

    auto connected = false;
    loop.blockOn(probe(&loop, socketPath.string(), &connected));

    if (!connected)
        return EXIT_FAILURE;
    std::println(stderr,
                 "contour attach: daemon on {} is alive; interactive attach lands with the "
                 "client protocol.",
                 socketPath.string());
    return EXIT_SUCCESS;
}

#else // _WIN32

int runDaemon(DaemonConfig const& /*config*/)
{
    std::println(stderr, "contour daemon: not supported on Windows yet");
    return EXIT_FAILURE;
}

int runAttachProbe(std::filesystem::path const& /*socketPath*/)
{
    std::println(stderr, "contour attach: not supported on Windows yet");
    return EXIT_FAILURE;
}

#endif // _WIN32

} // namespace muxserver
