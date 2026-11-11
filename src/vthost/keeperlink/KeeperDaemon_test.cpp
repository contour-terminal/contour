// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <vthost/Daemon.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/keeperlink/KeeperAttachment.hpp>
#include <vthost/keeperlink/KeeperChannel.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>
#include <vthost/testing/KeeperFixture.hpp>
#include <vthost/testing/ManualShutdownSignals.hpp>

using namespace std::chrono_literals;
using vthost::keeper::UniqueFd;
using vthost::keeperlink::KeeperChannel;
using vthost::keeperlink::ReceivedMessage;
using vthost::testing::HarnessStart;
using vthost::testing::KeeperFixture;

namespace keeper = vthost::keeper;
namespace fs = std::filesystem;

namespace
{

/// A directory for a daemon's sockets, left for listenUnix to create owner-only; removed with this.
struct TempDir
{
    fs::path path = fs::temp_directory_path() / std::format("contour-k-{}", std::random_device {}());

    TempDir() = default;
    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir()
    {
        auto ignored = std::error_code {};
        fs::remove_all(path, ignored);
    }
};

/// What a daemon told its keeper until it served.
struct ToldUntilServing
{
    std::vector<ReceivedMessage> listeners;
    std::optional<keeper::Serving> serving;
};

/// runDaemon() on a thread of its own, behind a keeper the test plays on the other end of the channel.
struct DaemonBehindKeeper
{
    TempDir tmp;
    core::platform::SteadyClock clock;
    std::shared_ptr<vthost::testing::ManualShutdownSignals> signals =
        std::make_shared<vthost::testing::ManualShutdownSignals>();
    std::vector<vthost::ShutdownCause> causes; ///< Written on the daemon's loop; read once it returned.
    /// Run on the daemon's loop from its beforeShutdown, before any listener closes; set before the shutdown.
    std::function<void()> onShutdown;
    vthost::DaemonConfig config;
    std::optional<KeeperChannel> keeperEnd;
    std::future<int> daemon;

    explicit DaemonBehindKeeper(vthost::KeeperSettings settings = {})
    {
        config.socketPath = tmp.path / "control";
        config.shutdownSignals = signals;
        config.keeper = settings;
        config.beforeShutdown = [this](vthost::ShutdownCause cause) {
            causes.push_back(cause);
            if (onShutdown)
                onShutdown();
        };

        auto ends = std::array { -1, -1 };
        // Close-on-exec set after the fact: macOS has no SOCK_CLOEXEC.
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0);
        for (auto const fd: ends)
            REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
        keeperEnd.emplace(UniqueFd { ends[1] });
        // The keeper speaks first; the daemon's handshake reads what it said.
        send(keeper::Hello {});
        send(keeper::Launch {
            .continuity = keeper::LaunchContinuity::Fresh, .options = {}, .originalDescriptorLimit = 0 });
        auto attachment = vthost::keeperlink::attachToKeeper(UniqueFd { ends[0] }, clock, 5s);
        REQUIRE(attachment.has_value());
        CHECK(attachment->continuity == keeper::LaunchContinuity::Fresh);
        CHECK(std::holds_alternative<keeper::Hello>(*receive(clock.now() + 5s).message));

        daemon = std::async(std::launch::async, [this, attached = std::move(*attachment)]() mutable {
            return vthost::runDaemon(config, std::nullopt, std::move(attached));
        });
    }

    DaemonBehindKeeper(DaemonBehindKeeper const&) = delete;
    DaemonBehindKeeper& operator=(DaemonBehindKeeper const&) = delete;
    DaemonBehindKeeper(DaemonBehindKeeper&&) = delete;
    DaemonBehindKeeper& operator=(DaemonBehindKeeper&&) = delete;

    ~DaemonBehindKeeper()
    {
        // Whatever the test did, the daemon ends before the future's destructor waits for it.
        signals->fire();
        keeperEnd.reset();
    }

    void send(keeper::Message const& message)
    {
        REQUIRE(keeperEnd->sendNow(message, {}, clock, clock.now() + 5s).has_value());
    }

    [[nodiscard]] ReceivedMessage receive(core::platform::SteadyTimePoint deadline)
    {
        auto received = keeperEnd->receive(clock, deadline);
        REQUIRE(received.has_value());
        REQUIRE(received->message.has_value());
        return std::move(*received);
    }

    /// Reads what the daemon says until it serves; heartbeats aside.
    [[nodiscard]] ToldUntilServing untilServing()
    {
        // One deadline for all: the heartbeats that come meanwhile must not keep a daemon that never serves
        // from failing the test.
        auto const deadline = clock.now() + 10s;
        auto told = ToldUntilServing {};
        while (!told.serving)
        {
            auto received = receive(deadline);
            if (auto const* serving = std::get_if<keeper::Serving>(&*received.message))
                told.serving = *serving;
            else if (std::holds_alternative<keeper::Listener>(*received.message))
                told.listeners.push_back(std::move(received));
        }
        return told;
    }

    /// @return The next message that is not a heartbeat.
    [[nodiscard]] ReceivedMessage nextButHeartbeats()
    {
        auto const deadline = clock.now() + 10s;
        while (true)
        {
            auto received = receive(deadline);
            if (!std::holds_alternative<keeper::Heartbeat>(*received.message))
                return received;
        }
    }

    /// Waits for runDaemon() to return.
    /// @return Its exit code.
    [[nodiscard]] int awaitEnd()
    {
        REQUIRE(daemon.wait_for(10s) == std::future_status::ready);
        return daemon.get();
    }
};

/// @return Whether @p fd is a stream socket, and one that listens at @p path takes a connection. Asked by
///         connecting rather than by SO_ACCEPTCONN, which macOS's getsockopt() does not answer.
bool listensAt(int fd, std::string const& path)
{
    auto type = 0;
    auto size = static_cast<socklen_t>(sizeof(type));
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM)
        return false;
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        return false;
    std::ranges::copy(path, std::begin(address.sun_path));
    auto const client = UniqueFd { ::socket(AF_UNIX, SOCK_STREAM, 0) };
    return client.isOpen()
           && ::connect(client.get(), reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0;
}

} // namespace

TEST_CASE("the daemon deposits every bound listener", "[keeper][daemon][afunix]")
{
    auto d = DaemonBehindKeeper {};
    auto const told = d.untilServing();

    auto paths = std::map<std::string, std::string> {};
    for (auto const& received: told.listeners)
    {
        auto const& listener = std::get<keeper::Listener>(*received.message);
        REQUIRE(received.fds.size() == 1);
        CHECK(listensAt(received.fds.front().get(), listener.path));
        CHECK_FALSE(listener.tlsIdentity.has_value());
        paths.emplace(listener.name, listener.path);
    }
    CHECK(paths
          == std::map<std::string, std::string> {
              { "control", core::pathToUtf8(d.config.socketPath) },
              { "native", core::pathToUtf8(vthost::nativeSocketPath(d.config.socketPath)) },
              { "imsg", core::pathToUtf8(vthost::tmuxSocketPath(d.config.socketPath)) },
          });
}

TEST_CASE("Serving carries the keeper settings", "[keeper][daemon][afunix]")
{
    auto d = DaemonBehindKeeper { vthost::KeeperSettings { .hungRestart = keeper::HungRestart::Off,
                                                           .heartbeatTimeout = 7'000ms } };
    auto const told = d.untilServing();
    REQUIRE(told.serving.has_value());
    CHECK(*told.serving
          == keeper::Serving { .hungRestart = keeper::HungRestart::Off, .heartbeatTimeoutMs = 7'000 });
    // And from then on, that it is alive.
    CHECK(std::holds_alternative<keeper::Heartbeat>(*d.receive(d.clock.now() + 5s).message));
}

TEST_CASE("a daemon tells its keeper it is alive before it serves", "[keeper][daemon][afunix]")
{
    // From the link on, not from Serving on: the keeper judges a start by its heartbeats once one came, so
    // that a start that restores at length is not taken for hung.
    auto d = DaemonBehindKeeper {};
    CHECK(std::holds_alternative<keeper::Heartbeat>(*d.receive(d.clock.now() + 10s).message));
    REQUIRE(d.untilServing().serving.has_value());
}

TEST_CASE("a lost keeper shuts the daemon down with KeeperLost", "[keeper][daemon][afunix]")
{
    auto d = DaemonBehindKeeper {};
    std::ignore = d.untilServing();
    d.keeperEnd.reset();
    CHECK(d.awaitEnd() == EXIT_SUCCESS);
    CHECK(d.causes == std::vector { vthost::ShutdownCause::KeeperLost });
    // Left to the keeper, even one that is gone: a stale file is replaced by the next start, while a file the
    // daemon removed as it closed could have been another daemon's, bound at the path meanwhile.
    CHECK(fs::exists(vthost::nativeSocketPath(d.config.socketPath)));
}

TEST_CASE("a daemon that shuts down tells its keeper first", "[keeper][daemon][afunix]")
{
    auto d = DaemonBehindKeeper {};
    std::ignore = d.untilServing();
    d.signals->fire();
    auto told = std::optional<keeper::Shutdown> {};
    while (!told)
    {
        auto const received = d.receive(d.clock.now() + 5s);
        REQUIRE(received.message);
        if (auto const* shutdown = std::get_if<keeper::Shutdown>(&*received.message))
            told = *shutdown;
    }
    CHECK(told->cause == std::to_underlying(vthost::ShutdownCause::Signal));
    // Review Focus 2: the daemon's listeners stay until the keeper took note, by when it let go of the label,
    // so that nothing still holds the label once the socket stops answering.
    std::this_thread::sleep_for(200ms);
    CHECK(fs::exists(vthost::nativeSocketPath(d.config.socketPath)));
    d.send(keeper::ShutdownNoted {});
    CHECK(d.awaitEnd() == EXIT_SUCCESS);
    CHECK(d.causes == std::vector { vthost::ShutdownCause::Signal });
    CHECK(fs::exists(vthost::nativeSocketPath(d.config.socketPath))); // the keeper removes it
}

TEST_CASE("a daemon whose keeper takes no note of its shutdown ends after the bound",
          "[keeper][daemon][afunix]")
{
    auto d = DaemonBehindKeeper {};
    std::ignore = d.untilServing();
    auto const started = std::chrono::steady_clock::now();
    d.signals->fire();
    CHECK(d.awaitEnd() == EXIT_SUCCESS);
    CHECK(std::chrono::steady_clock::now() - started >= vthost::keeperlink::ShutdownNoteBound);
    CHECK(fs::exists(vthost::nativeSocketPath(d.config.socketPath))); // the keeper removes it
}

TEST_CASE("contour daemon serves through a keeper and the daemon is its child", "[keeper][daemon][afunix]")
{
    auto f = KeeperFixture {};
    auto const daemon = f.daemonPid();
    REQUIRE(daemon > 0);
    CHECK(daemon != f.keeperPid());
    CHECK(vthost::testing::parentProcessOf(daemon) == f.keeperPid());
    CHECK(f.launches() == std::vector { daemon });
}

TEST_CASE("a second keeper on a held label fails without starting a daemon", "[keeper][daemon][afunix]")
{
    auto f = KeeperFixture {};
    struct stat before {};
    REQUIRE(::stat(f.socketPath().c_str(), &before) == 0);

    auto const second = f.runSecondKeeper(10s);
    REQUIRE(second.has_value());
    CHECK(*second == vthost::testing::HarnessKeeperStartFailureStatus); // refused at the label lock

    struct stat after {};
    REQUIRE(::stat(f.socketPath().c_str(), &after) == 0);
    CHECK(after.st_ino == before.st_ino);
    CHECK(f.launches().size() == 1);
    CHECK(f.daemonPid() == f.launches().front()); // the first daemon serves on
}

TEST_CASE("an invalid config fails the first start without a restart", "[keeper][daemon][afunix]")
{
    auto f = KeeperFixture { HarnessStart::Fails };
    auto const exited = f.awaitKeeperExit(15s);
    INFO(f.log());
    REQUIRE(exited.has_value());
    CHECK(*exited == vthost::testing::HarnessStartFailureStatus); // the daemon's own status
    CHECK(f.launches().size() == 1);
}

TEST_CASE("status reports the daemon and the keeper", "[keeper][daemon][afunix]")
{
    auto f = KeeperFixture {};
    auto const status = f.client()->status();
    REQUIRE(status.has_value());
    CHECK(status->keeperProcessId == static_cast<uint64_t>(f.keeperPid()));
    CHECK(std::cmp_equal(status->processId, f.daemonPid()));
    CHECK(vthost::testing::parentProcessOf(f.daemonPid()) == f.keeperPid());
}

#ifdef __linux__
TEST_CASE("the keeper is not dumpable: no other process of the user's reads its descriptors",
          "[keeper][daemon][afunix]")
{
    if (::getuid() == 0)
        SKIP("root reads every process's descriptors");
    auto f = KeeperFixture {};
    // A process that is not dumpable has its /proc entries owned by root.
    struct stat keeper {};
    REQUIRE(::stat(std::format("/proc/{}/fd", f.keeperPid()).c_str(), &keeper) == 0);
    CHECK(keeper.st_uid == 0);
    // Its daemon, dumpable again by its exec, is the user's as any process is.
    struct stat daemon {};
    REQUIRE(::stat(std::format("/proc/{}/fd", f.daemonPid()).c_str(), &daemon) == 0);
    CHECK(daemon.st_uid == ::getuid());
}
#endif
