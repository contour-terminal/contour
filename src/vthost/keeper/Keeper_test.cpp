// SPDX-License-Identifier: Apache-2.0
#include <core/codec/Fields.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <vthost/keeper/Codec.hpp>
#include <vthost/keeper/Keeper.hpp>
#include <vthost/keeper/Ports.hpp>

using namespace std::chrono_literals;
using namespace vthost::keeper;

namespace
{

// {{{ descriptors the tests can watch

/// A file's identity: its device and its number there.
using FileIdentity = std::pair<dev_t, ino_t>;

/// @return The identity of the file @p fd is open on.
FileIdentity identityOf(int fd)
{
    struct stat status {};
    REQUIRE(::fstat(fd, &status) == 0);
    return { status.st_dev, status.st_ino };
}

/// A pipe whose write end a test hands to the keeper and whose read end tells whether the keeper closed it.
struct Pipe
{
    UniqueFd reader;
    UniqueFd writer;
    FileIdentity given {}; ///< The write end's identity, once given away.

    /// @return The write end, for the keeper to own.
    [[nodiscard]] int give()
    {
        given = identityOf(writer.get());
        return writer.release();
    }

    /// Whether @p fd is open on the write end given away. Asked of that end itself: a pipe's two ends are one
    /// file on Linux, but two on macOS.
    [[nodiscard]] bool isGiven(int fd) const { return identityOf(fd) == given; }

    /// @return Whether every write end is closed: the reader sees end-of-file.
    [[nodiscard]] bool writerClosed() const
    {
        auto byte = char {};
        return ::read(reader.get(), &byte, 1) == 0;
    }
};

Pipe makePipe()
{
    auto fds = std::array<int, 2> {};
    REQUIRE(::pipe(fds.data()) == 0);
    for (auto const fd: fds)
        REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    REQUIRE(::fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    return Pipe { .reader = UniqueFd { fds[0] }, .writer = UniqueFd { fds[1] } };
}

// }}}
// {{{ fakes

/// One frame the keeper sent, decoded, with duplicates of its descriptors.
struct Sent
{
    Message message;
    std::vector<UniqueFd> fds;
};

/// What the keeper sent on one channel; outlives the channel.
struct ChannelRecord
{
    std::vector<Sent> sent;

    template <typename T>
    [[nodiscard]] std::vector<Sent const*> all() const
    {
        auto result = std::vector<Sent const*> {};
        for (auto const& item: sent)
            if (std::holds_alternative<T>(item.message))
                result.push_back(&item);
        return result;
    }

    template <typename T>
    [[nodiscard]] T const& only() const
    {
        auto const found = all<T>();
        REQUIRE(found.size() == 1);
        return std::get<T>(found.front()->message);
    }
};

class FakeChannel final: public Channel
{
  public:
    explicit FakeChannel(std::shared_ptr<ChannelRecord> record): _record { std::move(record) } {}

    void send(Frame frame) override
    {
        auto message = decode(frame.body);
        REQUIRE(message.has_value());
        auto fds = std::vector<UniqueFd> {};
        for (auto const fd: frame.fds)
        {
            auto duplicate = UniqueFd { ::fcntl(fd, F_DUPFD_CLOEXEC, 0) };
            REQUIRE(duplicate.isOpen());
            fds.push_back(std::move(duplicate));
        }
        _record->sent.push_back(Sent { .message = std::move(*message), .fds = std::move(fds) });
    }

    [[nodiscard]] int pollHandle() const override { return -1; }

  private:
    std::shared_ptr<ChannelRecord> _record;
};

class FakeLauncher final: public DaemonLauncher
{
  public:
    std::vector<std::filesystem::path> images;
    std::vector<std::shared_ptr<ChannelRecord>> channels;
    int nextPid = 100;
    std::vector<std::size_t> failingAttempts; ///< Launch attempts, counted from 0, that fail.

    [[nodiscard]] std::expected<LaunchedDaemon, SpawnError> launch(
        std::filesystem::path const& image) override
    {
        auto const attempt = images.size();
        images.push_back(image);
        if (std::ranges::contains(failingAttempts, attempt))
            return std::unexpected { SpawnError::ForkFailed };
        auto record = std::make_shared<ChannelRecord>();
        channels.push_back(record);
        return LaunchedDaemon { .pid = nextPid++,
                                .channel = std::make_unique<FakeChannel>(std::move(record)) };
    }

    [[nodiscard]] ChannelRecord const& channel(std::size_t index) const
    {
        REQUIRE(index < channels.size());
        return *channels[index];
    }
};

class FakeSpawner final: public ShellSpawner
{
  public:
    struct Request
    {
        Spawn spawn;
        std::size_t descriptorCount = 0;
    };

    std::vector<Request> requests;
    std::deque<UniqueFd> processHandles; ///< Handed out in turn; none when empty.
    int nextPid = 2000;

    [[nodiscard]] std::expected<SpawnedChild, SpawnError> spawn(Spawn const& request,
                                                                std::span<int const> descriptors) override
    {
        requests.push_back(Request { .spawn = request, .descriptorCount = descriptors.size() });
        auto handle = UniqueFd {};
        if (!processHandles.empty())
        {
            handle = std::move(processHandles.front());
            processHandles.pop_front();
        }
        return SpawnedChild { .pid = nextPid++, .processHandle = std::move(handle) };
    }
};

using Step = std::function<std::vector<Event>()>;

Event eventOf(EventKind kind)
{
    return Event { .kind = kind, .frame = std::nullopt, .pid = 0, .status = {}, .generation = {} };
}

/// Replays a script of steps, one per wait. Frames and closes a step leaves unstamped come from the newest
/// channel. When the script is done it winds the world down: a stop signal, then every channel closed and
/// every watched process reaped, so that every run ends.
class FakeEvents final: public EventSource
{
  public:
    core::platform::SteadyTimePoint lastDeadline {};

    void then(Step step) { _steps.push_back(std::move(step)); }

    [[nodiscard]] std::size_t stepsLeft() const noexcept { return _steps.size(); }

    void watchChannel(Channel& channel, DaemonGeneration generation) override
    {
        _channels.emplace_back(&channel, generation);
        _newest = generation;
    }
    void unwatchChannel(Channel& channel) override
    {
        std::erase_if(_channels, [&](auto const& entry) { return entry.first == &channel; });
    }
    void watchPid(int pid, Reaping reaping) override
    {
        _live.push_back(pid);
        if (reaping == Reaping::ByKeeper)
            _keptByKeeper.push_back(pid);
    }

    void reap(int pid) override
    {
        // Only a process left to the keeper, reported ended and not yet reaped: once each.
        REQUIRE(std::ranges::contains(_zombies, pid));
        std::erase(_zombies, pid);
        reaps.push_back(pid);
    }

    /// The pids the keeper reaped, in order.
    std::vector<int> reaps;

    /// @return Whether @p pid was reaped by the keeper already.
    [[nodiscard]] bool isReaped(int pid) const { return std::ranges::contains(reaps, pid); }

    /// @return The processes left to the keeper that ended and that it has not reaped.
    [[nodiscard]] std::vector<int> const& zombies() const noexcept { return _zombies; }

    [[nodiscard]] std::vector<Event> wait(core::platform::SteadyTimePoint deadline) override
    {
        lastDeadline = deadline;
        auto events = std::vector<Event> {};
        if (!_steps.empty())
        {
            auto step = std::move(_steps.front());
            _steps.pop_front();
            events = step();
        }
        else
            events = windDown();
        for (auto& event: events)
        {
            if (event.kind == EventKind::Ended)
            {
                std::erase(_live, event.pid);
                if (std::ranges::contains(_keptByKeeper, event.pid))
                    _zombies.push_back(event.pid);
            }
            auto const fromChannel = event.kind == EventKind::Frame || event.kind == EventKind::ChannelClosed;
            if (fromChannel && event.generation == DaemonGeneration {})
                event.generation = _newest;
        }
        return events;
    }

  private:
    std::vector<Event> windDown()
    {
        ++_windDownTurns;
        if (_windDownTurns > 8)
            throw std::runtime_error("the keeper did not end");
        if (_windDownTurns == 1)
            return { eventOf(EventKind::StopSignal) };
        auto events = std::vector<Event> {};
        for (auto const& [channel, generation]: _channels)
        {
            auto closed = eventOf(EventKind::ChannelClosed);
            closed.generation = generation;
            events.push_back(closed);
        }
        for (auto const pid: _live)
            events.push_back(Event { .kind = EventKind::Ended,
                                     .frame = std::nullopt,
                                     .pid = pid,
                                     .status = {},
                                     .generation = {} });
        return events;
    }

    std::deque<Step> _steps;
    std::vector<std::pair<Channel*, DaemonGeneration>> _channels;
    DaemonGeneration _newest {};
    std::vector<int> _keptByKeeper;
    std::vector<int> _zombies;
    std::vector<int> _live;
    int _windDownTurns = 0;
};

class FakeSignaller final: public Signaller
{
  public:
    explicit FakeSignaller(FakeEvents const& events): _events { events } {}

    std::vector<std::pair<int, KeeperSignal>> sent;

    /// @return What was sent to @p pid, in order.
    [[nodiscard]] std::vector<KeeperSignal> to(int pid) const
    {
        auto signals = std::vector<KeeperSignal> {};
        for (auto const& [target, signal]: sent)
            if (target == pid)
                signals.push_back(signal);
        return signals;
    }

    void send(int pid, KeeperSignal signal) override
    {
        // A reaped pid may belong to anyone by now.
        CHECK(!_events.isReaped(pid));
        sent.emplace_back(pid, signal);
    }

  private:
    FakeEvents const& _events;
};

class FakeBudget final: public DescriptorBudget
{
  public:
    std::size_t free = 1000;

    [[nodiscard]] std::size_t available() const override { return free; }
};

class FakeLabelLock final: public LabelLock
{
  public:
    explicit FakeLabelLock(std::function<void()> onRelease): _onRelease { std::move(onRelease) } {}
    FakeLabelLock(FakeLabelLock const&) = delete;
    FakeLabelLock& operator=(FakeLabelLock const&) = delete;
    FakeLabelLock(FakeLabelLock&&) = delete;
    FakeLabelLock& operator=(FakeLabelLock&&) = delete;
    ~FakeLabelLock() override { _onRelease(); }

  private:
    std::function<void()> _onRelease;
};

// }}}
// {{{ harness

std::filesystem::path daemonImage()
{
    return "/opt/contour/bin/contour";
}

std::vector<uint8_t> launchOptions()
{
    return { 1, 7, 9 };
}
constexpr auto FirstDaemon = 100;
constexpr auto FirstShell = 2000;

struct Harness
{
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} };
    FakeLauncher launcher;
    FakeSpawner spawner;
    FakeEvents events;
    FakeSignaller signaller { events };
    FakeBudget budget;
    std::function<void()> onLockReleased = [] {
    };
    std::optional<KeeperOutcome> outcome;
    int exitStatus = -1;
    Keeper const* running = nullptr; ///< The keeper run() runs, while it runs: what a step inspects.

    void run(RestartPolicyConfig policy = {})
    {
        auto keeper = Keeper { KeeperPorts { .launcher = launcher,
                                             .spawner = spawner,
                                             .events = events,
                                             .signaller = signaller,
                                             .budget = budget,
                                             .clock = clock },
                               std::make_unique<FakeLabelLock>([this] { onLockReleased(); }),
                               policy,
                               daemonImage(),
                               launchOptions() };
        running = &keeper;
        outcome = keeper.run();
        running = nullptr;
        exitStatus = keeper.exitStatus();
        CHECK(events.stepsLeft() == 0);
        CHECK(events.zombies().empty()); // every daemon that ended was reaped
    }

    /// A step that lets @p delta pass and wakes the keeper for its timer.
    Step after(core::platform::SteadyDuration delta)
    {
        return [this, delta] {
            clock.advance(delta);
            return std::vector { eventOf(EventKind::Timer) };
        };
    }
};

Event frameOf(Message const& message, std::vector<int> fds = {})
{
    return Event { .kind = EventKind::Frame,
                   .frame = Frame { .body = encode(message), .fds = std::move(fds) },
                   .pid = 0,
                   .status = {},
                   .generation = {} };
}

Event endOf(int pid, WireExitStatus status = {})
{
    return Event {
        .kind = EventKind::Ended, .frame = std::nullopt, .pid = pid, .status = status, .generation = {}
    };
}

/// A daemon's end as the event source reports it: its channel closes, then it is reaped.
std::vector<Event> ended(int pid, WireExitStatus status = {})
{
    return { eventOf(EventKind::ChannelClosed), endOf(pid, status) };
}

/// A frame from the channel of daemon generation @p generation.
Event frameFrom(DaemonGeneration generation, Message const& message, std::vector<int> fds = {})
{
    auto event = frameOf(message, std::move(fds));
    event.generation = generation;
    return event;
}

Step deliver(std::vector<Event> events)
{
    return [events = std::move(events)]() mutable {
        return std::move(events);
    };
}

Step check(std::function<void()> assertions, std::vector<Event> then = {})
{
    return [assertions = std::move(assertions), then = std::move(then)]() mutable {
        assertions();
        return std::move(then);
    };
}

/// A Spawn whose child gets @p child as descriptor 0 and whose keeper keeps @p kept, in order.
Event spawnOf(uint64_t request, Pipe& child, std::vector<std::pair<KeptDescriptor, Pipe*>> const& kept = {})
{
    auto spawn = Spawn {
        .request = request, .argv = { "/bin/sh" }, .executablePath = "/bin/sh", .childFds = { 0 }, .kept = {}
    };
    auto fds = std::vector { child.give() };
    for (auto const& [role, pipe]: kept)
    {
        spawn.kept.push_back(role);
        fds.push_back(pipe->give());
    }
    return frameOf(spawn, std::move(fds));
}

constexpr auto Killed = WireExitStatus { .kind = ExitKind::Signal, .value = 9 };
constexpr auto Segfault = WireExitStatus { .kind = ExitKind::Signal, .value = 11 };

// }}}

} // namespace

TEST_CASE("a Spawn is forked and answered with Spawned", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    auto pidfd = makePipe();
    h.spawner.processHandles.emplace_back(pidfd.give());
    h.events.then(deliver({ spawnOf(7, child, { { KeptDescriptor::Master, &master } }) }));
    h.events.then(check([&] {
        auto const& channel = h.launcher.channel(0);
        CHECK(channel.only<Spawned>() == Spawned { .request = 7, .session = 1, .pid = FirstShell });
        auto const spawned = channel.all<Spawned>();
        REQUIRE(spawned.front()->fds.size() == 1);
        CHECK(pidfd.isGiven(spawned.front()->fds.front().get()));
        REQUIRE(h.spawner.requests.size() == 1);
        CHECK(h.spawner.requests.front().descriptorCount == 1);
        CHECK(h.spawner.requests.front().spawn.request == 7);
        CHECK(child.writerClosed());   // the child has its own; the keeper's copy is closed
        CHECK(!master.writerClosed()); // the master is kept
    }));
    h.run();
}

TEST_CASE("a reaped shell becomes Exited", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(deliver({ spawnOf(1, child) }));
    h.events.then(deliver({ endOf(FirstShell, Killed) }));
    h.events.then(check(
        [&] { CHECK(h.launcher.channel(0).only<Exited>() == Exited { .session = 1, .status = Killed }); }));
    h.run();
}

TEST_CASE("an exit while no daemon runs is kept for the inventory", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    h.events.then(
        deliver({ frameOf(Serving {}), spawnOf(1, child, { { KeptDescriptor::Master, &master } }) }));
    // The daemon crashes; the shell ends before the new daemon has adopted the inventory.
    h.events.then(deliver(
        { eventOf(EventKind::ChannelClosed), endOf(FirstDaemon, Segfault), endOf(FirstShell, Killed) }));
    h.events.then(deliver({ frameOf(Adopt {}) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).all<Exited>().empty());
        auto const& recovered = h.launcher.channel(1);
        CHECK(recovered.all<Exited>().empty());
        auto const& session = recovered.only<InventorySession>();
        CHECK(session.session == 1);
        CHECK(session.pid == FirstShell);
        CHECK(session.exited == Killed);
    }));
    h.run();
}

TEST_CASE("Release hangs up, kills after the grace and keeps reaping", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    auto fastPipe = makePipe();
    h.events.then(deliver({ spawnOf(
        1, child, { { KeptDescriptor::Master, &master }, { KeptDescriptor::FastPipeReader, &fastPipe } }) }));
    h.events.then(deliver({ frameOf(Release { .session = 1, .graceMs = 5'000 }) }));
    h.events.then(check([&] {
        CHECK(master.writerClosed());
        CHECK(fastPipe.writerClosed());
        CHECK(h.signaller.sent.empty());
        CHECK(h.events.lastDeadline == h.clock.now() + 5'000ms);
    }));
    h.events.then(h.after(5'000ms));
    h.events.then(check(
        [&] { CHECK(h.signaller.sent == std::vector { std::pair { FirstShell, KeeperSignal::Kill } }); },
        { endOf(FirstShell, Killed) }));
    h.events.then(deliver({ frameOf(Adopt {}) }));
    h.events.then(check([&] {
        auto const& channel = h.launcher.channel(0);
        CHECK(channel.all<Exited>().empty()); // the daemon let it go
        CHECK(channel.only<InventoryBegin>().sessionCount == 0);
        CHECK(channel.all<InventorySession>().empty());
    }));
    h.run();
}

TEST_CASE("Placed drops the gate", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto gate = makePipe();
    h.events.then(deliver({ spawnOf(1, child, { { KeptDescriptor::Gate, &gate } }) }));
    h.events.then(check([&] { CHECK(!gate.writerClosed()); }, { frameOf(Placed { .session = 1 }) }));
    h.events.then(check([&] { CHECK(gate.writerClosed()); }));
    h.run();
}

TEST_CASE("Adopt streams every session, listener and the newest structure", "[keeper]")
{
    auto h = Harness {};
    auto native = makePipe();
    auto tcp = makePipe();
    auto structure = makePipe();
    auto child1 = makePipe();
    auto master1 = makePipe();
    auto gate1 = makePipe();
    auto child2 = makePipe();
    auto master2 = makePipe();
    auto fast2 = makePipe();
    auto settle2 = makePipe();
    auto gate2 = makePipe();
    auto const tls = std::vector<uint8_t> { 4, 2 };
    h.events.then(deliver({
        frameOf(Listener { .name = "native", .tlsIdentity = std::nullopt, .path = "/run/n" },
                { native.give() }),
        frameOf(Listener { .name = "tcp", .tlsIdentity = tls, .path = {} }, { tcp.give() }),
        frameOf(Structure {}, { structure.give() }),
        frameOf(Serving {}),
        spawnOf(1, child1, { { KeptDescriptor::Master, &master1 }, { KeptDescriptor::Gate, &gate1 } }),
        spawnOf(2,
                child2,
                { { KeptDescriptor::Settle, &settle2 },
                  { KeptDescriptor::Master, &master2 },
                  { KeptDescriptor::FastPipeReader, &fast2 },
                  { KeptDescriptor::Gate, &gate2 } }),
        frameOf(Placed { .session = 1 }),
    }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(deliver({ frameOf(Adopt {}) }));
    h.events.then(check([&] {
        auto const& sent = h.launcher.channel(1).sent;
        // Hello, Launch, then the inventory in order.
        REQUIRE(sent.size() == 2 + 1 + 2 + 2 + 1);
        auto const* begin = std::get_if<InventoryBegin>(&sent[2].message);
        REQUIRE(begin);
        CHECK(*begin
              == InventoryBegin {
                  .daemonImagePath = daemonImage().string(), .sessionCount = 2, .listenerCount = 2 });
        REQUIRE(sent[2].fds.size() == 1);
        CHECK(structure.isGiven(sent[2].fds[0].get()));

        CHECK(std::get<InventoryListener>(sent[3].message)
              == InventoryListener { .name = "native", .tlsIdentity = std::nullopt, .path = "/run/n" });
        REQUIRE(sent[3].fds.size() == 1);
        CHECK(native.isGiven(sent[3].fds[0].get()));
        CHECK(std::get<InventoryListener>(sent[4].message)
              == InventoryListener { .name = "tcp", .tlsIdentity = tls, .path = {} });
        REQUIRE(sent[4].fds.size() == 1);
        CHECK(tcp.isGiven(sent[4].fds[0].get()));

        CHECK(std::get<InventorySession>(sent[5].message)
              == InventorySession { .session = 1,
                                    .pid = FirstShell,
                                    .exited = std::nullopt,
                                    .placed = Placement::Placed,
                                    .descriptors = { KeptDescriptor::Master } });
        REQUIRE(sent[5].fds.size() == 1);
        CHECK(master1.isGiven(sent[5].fds[0].get()));

        CHECK(std::get<InventorySession>(sent[6].message)
              == InventorySession { .session = 2,
                                    .pid = FirstShell + 1,
                                    .exited = std::nullopt,
                                    .placed = Placement::Parked,
                                    .descriptors = { KeptDescriptor::Master,
                                                     KeptDescriptor::FastPipeReader,
                                                     KeptDescriptor::Gate,
                                                     KeptDescriptor::Settle } });
        REQUIRE(sent[6].fds.size() == 4);
        CHECK(master2.isGiven(sent[6].fds[0].get()));
        CHECK(fast2.isGiven(sent[6].fds[1].get()));
        CHECK(gate2.isGiven(sent[6].fds[2].get()));
        CHECK(settle2.isGiven(sent[6].fds[3].get()));

        CHECK(std::holds_alternative<InventoryEnd>(sent[7].message));
    }));
    h.run();
}

TEST_CASE("a new Structure replaces and closes the previous one", "[keeper]")
{
    auto h = Harness {};
    auto first = makePipe();
    auto second = makePipe();
    h.events.then(deliver({ frameOf(Structure {}, { first.give() }) }));
    h.events.then(check([&] { CHECK(!first.writerClosed()); }, { frameOf(Structure {}, { second.give() }) }));
    h.events.then(check([&] {
        CHECK(first.writerClosed());
        CHECK(!second.writerClosed());
    }));
    h.run();
}

TEST_CASE("a spawn beyond the descriptor budget is refused", "[keeper]")
{
    auto h = Harness {};
    h.budget.free = SpawnDescriptorReserve;
    auto child = makePipe();
    auto master = makePipe();
    h.events.then(deliver({ spawnOf(7, child, { { KeptDescriptor::Master, &master } }) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<SpawnFailed>()
              == SpawnFailed { .request = 7, .error = SpawnError::TooManyDescriptors });
        CHECK(h.spawner.requests.empty());
        CHECK(child.writerClosed());
        CHECK(master.writerClosed());
    }));
    h.run();
}

TEST_CASE("a spawn that leaves the reserve free is accepted", "[keeper]")
{
    auto h = Harness {};
    h.budget.free = SpawnDescriptorReserve + 1;
    auto child = makePipe();
    h.events.then(deliver({ spawnOf(7, child) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).all<SpawnFailed>().empty());
        CHECK(h.launcher.channel(0).only<Spawned>().request == 7);
    }));
    h.run();
}

TEST_CASE("a Spawn whose descriptors do not match its table is refused", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto spawn = Spawn {
        .request = 3, .argv = {}, .executablePath = {}, .childFds = { 0, 1 }, .kept = {}
    }; // two child descriptors named, one sent
    h.events.then(deliver({ frameOf(spawn, { child.give() }) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<SpawnFailed>().error == SpawnError::Unknown);
        CHECK(h.spawner.requests.empty());
        CHECK(child.writerClosed());
    }));
    h.run();
}

TEST_CASE("missed heartbeats abort, continue, then kill", "[keeper]")
{
    auto h = Harness {};
    h.events.then(
        deliver({ frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 30'000 }) }));
    h.events.then([&] {
        h.clock.advance(20s);
        return std::vector { frameOf(Heartbeat {}) };
    });
    h.events.then(h.after(30s - 1ms));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    h.events.then(h.after(1ms));
    h.events.then(check([&] {
        CHECK(h.signaller.sent
              == std::vector { std::pair { FirstDaemon, KeeperSignal::Abort },
                               std::pair { FirstDaemon, KeeperSignal::Continue } });
    }));
    h.events.then(h.after(10s));
    h.events.then(check(
        [&] {
            REQUIRE(h.signaller.sent.size() == 3);
            CHECK(h.signaller.sent.back() == std::pair { FirstDaemon, KeeperSignal::Kill });
        },
        ended(FirstDaemon, Killed)));
    h.events.then(check([&] {
        REQUIRE(h.launcher.channels.size() == 2);
        CHECK(h.launcher.channel(1).only<Launch>().continuity == LaunchContinuity::Recovered);
    }));
    h.run();
}

TEST_CASE("with restart_hung off, missed heartbeats do nothing", "[keeper]")
{
    auto h = Harness {};
    h.events.then(
        deliver({ frameOf(Serving { .hungRestart = HungRestart::Off, .heartbeatTimeoutMs = 30'000 }) }));
    h.events.then(h.after(120s));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    h.run();
}

TEST_CASE("Shutdown releases the label lock before the shells are ended", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    auto listener = makePipe();
    auto lockReleasedWhileShellsRan = false;
    h.onLockReleased = [&] {
        lockReleasedWhileShellsRan = !master.writerClosed() && h.signaller.sent.empty();
    };
    h.events.then(deliver({ frameOf(Listener { .name = "native", .tlsIdentity = std::nullopt, .path = {} },
                                    { listener.give() }),
                            frameOf(Serving {}),
                            spawnOf(1, child, { { KeptDescriptor::Master, &master } }) }));
    h.events.then(deliver({ frameOf(Shutdown { .cause = 1 }) }));
    h.events.then(check(
        [&] {
            CHECK(lockReleasedWhileShellsRan);
            CHECK(listener.writerClosed());
            CHECK(!master.writerClosed()); // the shells wait for the daemon's exit
        },
        ended(FirstDaemon)));
    h.events.then(check([&] {
        CHECK(master.writerClosed());
        CHECK(h.signaller.sent.empty());
    }));
    h.events.then(h.after(3s));
    h.events.then(check(
        [&] { CHECK(h.signaller.sent == std::vector { std::pair { FirstShell, KeeperSignal::Kill } }); },
        { endOf(FirstShell, Killed) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.exitStatus == 0);
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a crashed daemon is relaunched with Recovered", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(check([&] {
        CHECK(h.launcher.images == std::vector { daemonImage(), daemonImage() });
        auto const& first = h.launcher.channel(0);
        CHECK(std::holds_alternative<Hello>(first.sent.at(0).message));
        CHECK(std::get<Launch>(first.sent.at(1).message)
              == Launch { .continuity = LaunchContinuity::Fresh, .options = launchOptions() });
        auto const& second = h.launcher.channel(1);
        CHECK(std::holds_alternative<Hello>(second.sent.at(0).message));
        CHECK(std::get<Launch>(second.sent.at(1).message)
              == Launch { .continuity = LaunchContinuity::Recovered, .options = launchOptions() });
    }));
    h.run();
}

TEST_CASE("a channel closed without Shutdown is a crash", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}), eventOf(EventKind::ChannelClosed) }));
    h.events.then(check(
        [&] { CHECK(h.signaller.sent == std::vector { std::pair { FirstDaemon, KeeperSignal::Kill } }); },
        { endOf(FirstDaemon, Killed) }));
    h.events.then(check([&] {
        REQUIRE(h.launcher.channels.size() == 2);
        CHECK(h.launcher.channel(1).only<Launch>().continuity == LaunchContinuity::Recovered);
    }));
    h.run();
}

TEST_CASE("a restart that never serves within the bound is killed as hung", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(h.after(30s));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon + 1) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
    }));
    h.run();
}

TEST_CASE("a first start that never serves exits with the daemon's status", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver(ended(FirstDaemon, WireExitStatus { .kind = ExitKind::Normal, .value = 3 })));
    h.run();
    CHECK(h.outcome == KeeperOutcome::DaemonFailedToStart);
    CHECK(h.exitStatus == 3);
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a stop signal is forwarded and the daemon's exit ends the keeper cleanly", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}), eventOf(EventKind::StopSignal) }));
    h.events.then(check(
        [&] {
            CHECK(h.signaller.sent == std::vector { std::pair { FirstDaemon, KeeperSignal::Terminate } });
        },
        ended(FirstDaemon)));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("Unsupported answers an unknown tag", "[keeper]")
{
    auto h = Harness {};
    auto writer = core::codec::Writer {};
    writer.varint(4000);
    h.events.then(deliver({ Event { .kind = EventKind::Frame,
                                    .frame = Frame { .body = std::move(writer).take(), .fds = {} },
                                    .pid = 0,
                                    .status = {},
                                    .generation = {} } }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<Unsupported>() == Unsupported { .tag = 4000 });
        CHECK(h.signaller.sent.empty()); // the link stays up
    }));
    h.run();
}

TEST_CASE("a newer daemon's enum value is answered Unsupported, and the link stays up", "[keeper]")
{
    auto h = Harness {};
    auto body = encode(SpawnFailed { .request = 1, .error = SpawnError::ForkFailed });
    REQUIRE(body.back() == 1); // the error's wire index, last
    body.back() = 9;           // a value a later version appended to SpawnError's table
    h.events.then(deliver({ Event { .kind = EventKind::Frame,
                                    .frame = Frame { .body = std::move(body), .fds = {} },
                                    .pid = 0,
                                    .status = {},
                                    .generation = {} } }));
    h.events.then(check([&] {
        auto const tag = Message { SpawnFailed {} }.index() + 1;
        CHECK(h.launcher.channel(0).only<Unsupported>() == Unsupported { .tag = tag });
        CHECK(h.signaller.sent.empty()); // the link stays up: the daemon is not killed
    }));
    h.run();
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a message with more descriptors than it may carry is answered Unsupported, and they are closed",
          "[keeper]")
{
    auto h = Harness {};
    auto surplus = makePipe();
    h.events.then(deliver({ frameOf(Heartbeat {}, { surplus.give() }) }));
    h.events.then(check([&] {
        auto const tag = Message { Heartbeat {} }.index() + 1;
        CHECK(h.launcher.channel(0).only<Unsupported>() == Unsupported { .tag = tag });
        CHECK(surplus.writerClosed());
        CHECK(h.signaller.sent.empty());
    }));
    h.run();
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a frame cut short breaks the link", "[keeper]")
{
    auto h = Harness {};
    auto body = encode(Exec { .executablePath = "/usr/bin/contour" });
    body.resize(body.size() - 3);
    h.events.then(deliver({ frameOf(Serving {}),
                            Event { .kind = EventKind::Frame,
                                    .frame = Frame { .body = std::move(body), .fds = {} },
                                    .pid = 0,
                                    .status = {},
                                    .generation = {} } }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).all<Unsupported>().empty());
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Kill });
    }));
    h.run();
}

TEST_CASE("a start that keeps beating is judged by its heartbeats, not by the startup bound", "[keeper]")
{
    auto h = Harness {};
    // The startup bound is 30 s; the daemon beats every 20 s for a minute and does not serve yet.
    for ([[maybe_unused]] auto const beat: std::views::iota(0, 3))
        h.events.then([&] {
            h.clock.advance(20s);
            return std::vector { frameOf(Heartbeat {}) };
        });
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    // Then it falls silent: hung once Serving's default heartbeat timeout passed since its last beat.
    h.events.then(h.after(DefaultHeartbeatTimeout - 1ms));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    h.events.then(h.after(1ms));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
    }));
    h.run();
}

TEST_CASE("a start's silences after its first beat may each last the startup bound", "[keeper]")
{
    // A user who raised startup_timeout for a slow restore: the start's silences are held to it, not to the
    // shorter heartbeat timeout.
    auto policy = RestartPolicyConfig {};
    policy.restartStartupBound = 120s;
    auto h = Harness {};
    h.events.then([&] {
        h.clock.advance(10s);
        return std::vector { frameOf(Heartbeat {}) };
    });
    h.events.then(h.after(120s - 1ms));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    h.events.then(h.after(1ms));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
    }));
    h.run(policy);
}

TEST_CASE("a keeper stopped for less than the heartbeat timeout does not judge the daemon on resuming",
          "[keeper]")
{
    auto h = Harness {};
    h.events.then(
        deliver({ frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 60'000 }) }));
    // Stopped mid-silence: its wait returns 45 s past the deadline, before the continued daemon's beat is
    // read.
    h.events.then(h.after(60s + 45s));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }, { frameOf(Heartbeat {}) }));
    h.run();
}

TEST_CASE("a keeper that was stopped itself does not take the daemon's silence for a hang", "[keeper]")
{
    auto h = Harness {};
    h.events.then(
        deliver({ frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 30'000 }) }));
    // Ctrl-Z stops the keeper with its daemon: its wait returns ten minutes past its deadline, before the
    // heartbeat the continued daemon sends is read.
    h.events.then(h.after(10min));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }, { frameOf(Heartbeat {}) }));
    // Measured afresh from there, a silence is judged again.
    h.events.then(h.after(30s));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
    }));
    h.run();
}

TEST_CASE("five quick crashes give up, retry once after the grace, then end everything", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    h.events.then(
        deliver({ frameOf(Serving {}), spawnOf(1, child, { { KeptDescriptor::Master, &master } }) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));     // restart at once
    h.events.then(deliver(ended(FirstDaemon + 1, Segfault))); // back off 1 s
    h.events.then(h.after(1s));
    h.events.then(deliver(ended(FirstDaemon + 2, Segfault))); // back off 2 s
    h.events.then(h.after(2s));
    h.events.then(deliver(ended(FirstDaemon + 3, Segfault))); // back off 4 s
    h.events.then(h.after(4s));
    h.events.then(deliver(ended(FirstDaemon + 4, Segfault))); // the fifth: give up
    h.events.then(check([&] {
        CHECK(h.launcher.channels.size() == 5);
        CHECK(!master.writerClosed()); // the shells are kept through the grace period
    }));
    h.events.then(h.after(120s));
    h.events.then(check([&] { CHECK(h.launcher.channels.size() == 6); }, ended(FirstDaemon + 5, Segfault)));
    h.events.then(check([&] { CHECK(master.writerClosed()); }));
    h.events.then(h.after(3s));
    h.events.then(check([&] { CHECK(h.signaller.to(FirstShell) == std::vector { KeeperSignal::Kill }); },
                        { endOf(FirstShell, Killed) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::GaveUp);
    CHECK(h.exitStatus != 0);
}

TEST_CASE("a daemon reaped before its channel closed is judged once, after the close", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver({ endOf(FirstDaemon, Segfault), eventOf(EventKind::ChannelClosed) }));
    h.events.then(check([&] {
        REQUIRE(h.launcher.channels.size() == 2);
        CHECK(h.signaller.to(FirstDaemon + 1).empty()); // the old close did not reach the new daemon
    }));
    // One crash, not two: the next one is a quick repeat that backs off rather than giving up early.
    h.events.then(deliver({ frameOf(Serving {}), endOf(FirstDaemon + 1, Segfault) }));
    h.events.then(
        check([&] { CHECK(h.launcher.channels.size() == 2); }, { eventOf(EventKind::ChannelClosed) }));
    h.events.then(check([&] { CHECK(h.launcher.channels.size() == 2); })); // backing off 1 s
    h.events.then(h.after(1s));
    h.events.then(check([&] { CHECK(h.launcher.channels.size() == 3); }));
    h.run();
}

TEST_CASE("a Shutdown read in the same batch as the daemon's reap ends cleanly", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver({ frameOf(Shutdown { .cause = 1 }), endOf(FirstDaemon) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a daemon reaped before it sent Shutdown is still judged by its frames", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver({ endOf(FirstDaemon), frameOf(Shutdown { .cause = 1 }) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a reaped daemon whose channel never closes is judged after the drain bound", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}), endOf(FirstDaemon, Segfault) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channels.size() == 1);
        CHECK(h.events.lastDeadline == h.clock.now() + ChannelDrainBound);
    }));
    h.events.then(h.after(ChannelDrainBound));
    h.events.then(check([&] { CHECK(h.launcher.channels.size() == 2); }));
    h.run();
}

TEST_CASE("a stale event of a dead daemon is ignored by its successor", "[keeper]")
{
    auto h = Harness {};
    auto lockReleased = false;
    h.onLockReleased = [&] {
        lockReleased = true;
    };
    auto structure = makePipe();
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(deliver({ frameFrom(DaemonGeneration { 1 }, Shutdown { .cause = 1 }),
                            frameFrom(DaemonGeneration { 1 }, Structure {}, { structure.give() }),
                            frameFrom(DaemonGeneration { 1 }, Adopt {}),
                            [] {
                                auto closed = eventOf(EventKind::ChannelClosed);
                                closed.generation = DaemonGeneration { 1 };
                                return closed;
                            }() }));
    h.events.then(check([&] {
        CHECK(!lockReleased);
        CHECK(structure.writerClosed()); // a stale frame's descriptors are closed, not kept
        CHECK(h.launcher.channel(1).all<InventoryBegin>().empty());
        CHECK(h.signaller.to(FirstDaemon + 1).empty());
    }));
    h.run();
}

TEST_CASE("a failed first launch ends the keeper at once with a non-zero status", "[keeper]")
{
    auto h = Harness {};
    h.launcher.failingAttempts = { 0 };
    h.run();
    CHECK(h.outcome == KeeperOutcome::DaemonFailedToStart);
    CHECK(h.exitStatus != 0);
    CHECK(h.launcher.channels.empty());
}

TEST_CASE("a failed restart launch counts as a crash", "[keeper]")
{
    auto h = Harness {};
    h.launcher.failingAttempts = { 1 };
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault))); // restart at once: that launch fails
    h.events.then(check([&] {
        CHECK(h.launcher.images.size() == 2);
        CHECK(h.launcher.channels.size() == 1);
    }));
    h.events.then(h.after(1s)); // a quick repeat: backed off 1 s
    h.events.then(check([&] {
        REQUIRE(h.launcher.channels.size() == 2);
        CHECK(h.launcher.channel(1).only<Launch>().continuity == LaunchContinuity::Recovered);
    }));
    h.run();
}

TEST_CASE("a first daemon that never serves within the bound is killed and fails the start", "[keeper]")
{
    auto h = Harness {};
    h.events.then(h.after(30s));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
    }));
    h.events.then(h.after(HungKillDelay));
    h.events.then(check([&] { CHECK(h.signaller.to(FirstDaemon).back() == KeeperSignal::Kill); },
                        ended(FirstDaemon, Killed)));
    h.run();
    CHECK(h.outcome == KeeperOutcome::DaemonFailedToStart);
    CHECK(h.exitStatus == 128 + 9);
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a heartbeat timeout below the minimum is raised to it", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 0 }) }));
    h.events.then(h.after(MinHeartbeatTimeout - 1ms));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    h.events.then(h.after(1ms));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
    }));
    h.run();
}

TEST_CASE("after Shutdown the keeper takes on no new shell or listener", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto listener = makePipe();
    h.events.then(deliver({ frameOf(Serving {}),
                            frameOf(Shutdown { .cause = 1 }),
                            spawnOf(4, child),
                            frameOf(Listener { .name = "late", .tlsIdentity = std::nullopt, .path = {} },
                                    { listener.give() }) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<SpawnFailed>()
              == SpawnFailed { .request = 4, .error = SpawnError::ShuttingDown });
        CHECK(h.spawner.requests.empty());
        CHECK(child.writerClosed());
        CHECK(listener.writerClosed());
    }));
    h.run();
}

TEST_CASE("a shell reaped within its Release grace is not killed", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(deliver({ spawnOf(1, child), frameOf(Release { .session = 1, .graceMs = 5'000 }) }));
    h.events.then(deliver({ endOf(FirstShell) }));
    h.events.then(h.after(10s));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstShell).empty());
        CHECK(h.launcher.channel(0).all<Exited>().empty());
    }));
    h.run();
}

TEST_CASE("Release of a session whose shell already exited forgets it at once", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(deliver({ spawnOf(1, child), endOf(FirstShell, Killed) }));
    h.events.then(deliver({ frameOf(Release { .session = 1, .graceMs = 5'000 }), frameOf(Adopt {}) }));
    h.events.then(h.after(10s));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<Exited>() == Exited { .session = 1, .status = Killed });
        CHECK(h.launcher.channel(0).only<InventoryBegin>().sessionCount == 0);
        CHECK(h.signaller.to(FirstShell).empty());
    }));
    h.run();
}

TEST_CASE("an exit told to a daemon that then crashed is in the inventory again, the session being held",
          "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(deliver({ frameOf(Serving {}), spawnOf(1, child), endOf(FirstShell, Killed) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(deliver({ frameOf(Adopt {}) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<Exited>() == Exited { .session = 1, .status = Killed });
        // The new daemon restores the session and must learn that its shell is gone.
        CHECK(h.launcher.channel(1).only<InventorySession>().exited == Killed);
    }));
    h.run();
}

TEST_CASE("a crashed daemon is reaped once, after the last signal it gets", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault))); // its close draws a Kill; then it ends
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Kill });
        CHECK(h.events.reaps == std::vector { FirstDaemon });
    }));
    h.events.then(deliver({ endOf(FirstDaemon + 1, Segfault) })); // ended before its channel closed
    h.events.then(check([&] { CHECK(h.events.reaps == std::vector { FirstDaemon }); }, // not before the drain
                        { eventOf(EventKind::ChannelClosed) }));
    h.events.then(check([&] {
        CHECK(h.events.reaps == std::vector { FirstDaemon, FirstDaemon + 1 });
        CHECK(h.signaller.to(FirstDaemon + 1).empty());
    }));
    h.run();
}

TEST_CASE("the frames after a broken one in the same batch are dropped", "[keeper]")
{
    auto h = Harness {};
    auto lockReleased = false;
    h.onLockReleased = [&] {
        lockReleased = true;
    };
    auto garbage = eventOf(EventKind::Frame);
    garbage.frame = Frame { .body = {}, .fds = {} }; // no tag at all: a protocol error
    h.events.then(deliver({ frameOf(Serving {}), garbage, frameOf(Shutdown { .cause = 1 }) }));
    h.events.then(check(
        [&] {
            CHECK(!lockReleased); // the Shutdown after the garbage was not acted on
            CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Kill });
        },
        ended(FirstDaemon, Killed)));
    h.events.then(check([&] {
        // A crash, not a shutdown: restarted.
        REQUIRE(h.launcher.channels.size() == 2);
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Kill }); // once
    }));
    h.run();
}

TEST_CASE("a Spawn from a daemon that already ended is refused", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(deliver({ frameOf(Serving {}), endOf(FirstDaemon, Segfault), spawnOf(5, child) }));
    h.events.then(check([&] {
        CHECK(h.spawner.requests.empty());
        CHECK(child.writerClosed());
        CHECK(h.launcher.channel(0).only<SpawnFailed>().error == SpawnError::ShuttingDown);
    }));
    h.run();
}

TEST_CASE("a Spawn from a daemon being killed as hung is refused", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(
        deliver({ frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 30'000 }) }));
    h.events.then(h.after(30s));
    h.events.then(deliver({ spawnOf(5, child) }));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
        CHECK(h.spawner.requests.empty());
        CHECK(child.writerClosed());
    }));
    h.run();
}

TEST_CASE("a stop while a reaped daemon's channel drains ends cleanly without a restart", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}), endOf(FirstDaemon, Segfault) }));
    h.events.then(deliver({ eventOf(EventKind::StopSignal) }));
    h.events.then(check([&] { CHECK(h.signaller.to(FirstDaemon).empty()); }, // it is gone: nothing to forward
                        { eventOf(EventKind::ChannelClosed) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.launcher.channels.size() == 1);
    CHECK(h.events.reaps == std::vector { FirstDaemon });
}

TEST_CASE("the keeper records the channel version of each image's Hello", "[keeper]")
{
    auto h = Harness {};
    h.events.then(check([&] { CHECK(h.running->daemonChannelVersion() == 0); },
                        { frameOf(Hello { .version = ChannelVersion + 4, .capabilities = 0 }) }));
    h.events.then(check([&] { CHECK(h.running->daemonChannelVersion() == ChannelVersion); },
                        { frameOf(Hello { .version = 0, .capabilities = 0 }) }));
    // A live upgrade's new image speaks its own.
    h.events.then(check([&] { CHECK(h.running->daemonChannelVersion() == 0); }));
    h.run();
}

TEST_CASE("a second Hello from the daemon is answered with a Launch for its new image", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}) }));
    h.events.then(check([&] {
        // The first Hello is the launched image's own: it has its Launch already.
        CHECK(h.launcher.channel(0).all<Launch>().size() == 1);
    }));
    h.events.then(deliver({ frameOf(Hello {}) }));
    h.events.then(check([&] {
        auto const launches = h.launcher.channel(0).all<Launch>();
        REQUIRE(launches.size() == 2);
        CHECK(std::get<Launch>(launches.back()->message)
              == Launch { .continuity = LaunchContinuity::HandedOver, .options = launchOptions() });
        CHECK(h.launcher.channel(0).all<Hello>().size() == 2);
        CHECK(h.launcher.images.size() == 1); // the same process: nothing was launched
        CHECK(h.signaller.sent.empty());
    }));
    h.run();
}

TEST_CASE("a listener deposited again under its name replaces the one held", "[keeper]")
{
    auto h = Harness {};
    auto first = makePipe();
    auto second = makePipe();
    h.events.then(deliver({
        frameOf(Listener { .name = "control", .tlsIdentity = std::nullopt, .path = "/run/c" },
                { first.give() }),
        frameOf(Serving {}),
    }));
    h.events.then(deliver({
        frameOf(Listener { .name = "control", .tlsIdentity = std::nullopt, .path = "/run/c" },
                { second.give() }),
    }));
    h.events.then(check([&] { CHECK(first.writerClosed()); }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(deliver({ frameOf(Adopt {}) }));
    h.events.then(check([&] {
        auto const& sent = h.launcher.channel(1).sent;
        auto const listeners = h.launcher.channel(1).all<InventoryListener>();
        REQUIRE(listeners.size() == 1);
        CHECK(std::get<InventoryListener>(listeners.front()->message)
              == InventoryListener { .name = "control", .tlsIdentity = std::nullopt, .path = "/run/c" });
        REQUIRE(listeners.front()->fds.size() == 1);
        CHECK(second.isGiven(listeners.front()->fds[0].get()));
        CHECK(std::get<InventoryBegin>(sent[2].message).listenerCount == 1);
    }));
    h.run();
}

TEST_CASE("a first start that closes its channel before it exits fails with its own status", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ eventOf(EventKind::ChannelClosed) }));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); },
                        { endOf(FirstDaemon, { .kind = ExitKind::Normal, .value = 3 }) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::DaemonFailedToStart);
    CHECK(h.exitStatus == 3);
    CHECK(h.signaller.sent.empty());
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a first start that closed its channel and lingers is killed after the drain bound", "[keeper]")
{
    auto h = Harness {};
    h.events.then(deliver({ eventOf(EventKind::ChannelClosed) }));
    h.events.then(h.after(ChannelDrainBound - 1ms));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    h.events.then(h.after(1ms));
    h.events.then(check([&] { CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Kill }); },
                        { endOf(FirstDaemon, Killed) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::DaemonFailedToStart);
    CHECK(h.launcher.channels.size() == 1);
}

TEST_CASE("a live upgrade's new image runs with its own hung-daemon settings", "[keeper]")
{
    auto h = Harness {};
    h.events.then(
        deliver({ frameOf(Hello {}),
                  frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 2'000 }) }));
    // The new image says Hello, is launched, and serves with restart_hung turned off.
    h.events.then(
        deliver({ frameOf(Hello {}),
                  frameOf(Serving { .hungRestart = HungRestart::Off, .heartbeatTimeoutMs = 2'000 }) }));
    h.events.then(h.after(10s));
    h.events.then(check([&] { CHECK(h.signaller.sent.empty()); }));
    h.run();
}

TEST_CASE("a live upgrade's new image that shortens the heartbeat timeout is held to it", "[keeper]")
{
    auto h = Harness {};
    h.events.then(
        deliver({ frameOf(Hello {}),
                  frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 30'000 }) }));
    h.events.then(
        deliver({ frameOf(Hello {}),
                  frameOf(Serving { .hungRestart = HungRestart::On, .heartbeatTimeoutMs = 2'000 }) }));
    h.events.then(h.after(2s));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Abort, KeeperSignal::Continue });
    }));
    h.run();
}
