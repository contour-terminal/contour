// SPDX-License-Identifier: Apache-2.0
#include <core/codec/Fields.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
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
        std::optional<int> executable;
    };

    std::vector<Request> requests;
    std::deque<UniqueFd> processHandles; ///< Handed out in turn; none when empty.
    int nextPid = 2000;

    [[nodiscard]] std::expected<SpawnedChild, SpawnError> spawn(Spawn const& request,
                                                                std::span<int const> descriptors,
                                                                std::optional<int> executable) override
    {
        requests.push_back(
            Request { .spawn = request, .descriptorCount = descriptors.size(), .executable = executable });
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
    void watchPid(WatchedProcess process) override
    {
        auto const pid = process.pid;
        watches.push_back(process);
        _watched.push_back(pid);
        // Its end may be in the batch that started it (a step standing for two waits): reported once.
        if (!std::ranges::contains(_ended, pid))
            _live.push_back(pid);
    }

    void reap(int pid) override
    {
        // Only a process left to the keeper, reported ended and not yet reaped: once each.
        REQUIRE(std::ranges::contains(zombies(), pid));
        reaps.push_back(pid);
    }

    /// The pids the keeper reaped, in order.
    std::vector<int> reaps;
    /// Every watchPid(), in order.
    std::vector<WatchedProcess> watches;

    /// @return Whether @p pid was reaped by the keeper already.
    [[nodiscard]] bool isReaped(int pid) const { return std::ranges::contains(reaps, pid); }

    /// @return The processes left to the keeper that ended and that it has not reaped.
    /// A process is watched before its end can be reported; a step that reports a shell's end beside the
    /// Spawn that starts it stands for two waits, so it counts as watched once the keeper watches it.
    [[nodiscard]] std::vector<int> zombies() const
    {
        return std::ranges::to<std::vector>(_ended | std::views::filter([this](int pid) {
                                                return std::ranges::contains(_watched, pid) && !isReaped(pid);
                                            }));
    }

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
                _ended.push_back(event.pid);
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
    std::vector<int> _watched; ///< Every process ever watched: each ended one is the keeper's to reap.
    std::vector<int> _ended;   ///< Every process reported ended.
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
    uint64_t original = 1024;

    mutable int asked = 0; ///< How often available() was asked.

    [[nodiscard]] std::size_t available() const override
    {
        ++asked;
        return free;
    }
    [[nodiscard]] uint64_t originalSoftLimit() const override { return original; }
};

/// Picks what the test says: the committed image until it names a newer one.
class FakeRestartImages final: public RestartImages
{
  public:
    std::optional<std::filesystem::path> newer; ///< What a pick returns from now on; none for the committed.
    std::vector<std::filesystem::path> picks;   ///< The committed image each pick was asked about.
    std::set<std::filesystem::path> images;     ///< The paths hold() takes for images.
    std::shared_ptr<int> heldCount = std::make_shared<int>(0); ///< How many holds are alive.

    [[nodiscard]] std::filesystem::path pick(std::filesystem::path const& committed) override
    {
        picks.push_back(committed);
        return newer.value_or(committed);
    }

    [[nodiscard]] std::expected<std::unique_ptr<HeldImage>, RestartImageRefusal> hold(
        std::filesystem::path const& executable) override
    {
        if (!images.contains(executable))
            return std::unexpected { RestartImageRefusal::NotAnImage };
        return std::make_unique<CountedHold>(heldCount);
    }

  private:
    /// Counts itself alive in the count it is given.
    class CountedHold final: public HeldImage
    {
      public:
        explicit CountedHold(std::shared_ptr<int> count): _count { std::move(count) } { ++*_count; }
        CountedHold(CountedHold const&) = delete;
        CountedHold& operator=(CountedHold const&) = delete;
        CountedHold(CountedHold&&) = delete;
        CountedHold& operator=(CountedHold&&) = delete;
        ~CountedHold() override { --*_count; }

      private:
        std::shared_ptr<int> _count;
    };
};

class FakeSocketFiles final: public SocketFiles
{
  public:
    std::vector<std::filesystem::path> removed;
    std::function<void()> onRemove = [] {
    };

    void remove(std::filesystem::path const& path) override
    {
        removed.push_back(path);
        onRemove();
    }
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
    FakeRestartImages images;
    FakeSocketFiles socketFiles;
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
                                             .clock = clock,
                                             .images = images,
                                             .socketFiles = socketFiles },
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

TEST_CASE("the spawns of one wait count the budget once, and what each keeps", "[keeper]")
{
    auto h = Harness {};
    // Room for two spawns' pidfds above the reserve, not three.
    h.budget.free = SpawnDescriptorReserve + 2;
    auto pidfds = std::array { makePipe(), makePipe(), makePipe() };
    for (auto& pidfd: pidfds)
        h.spawner.processHandles.emplace_back(pidfd.give());
    auto first = makePipe();
    auto second = makePipe();
    auto third = makePipe();
    h.events.then(deliver({ spawnOf(1, first), spawnOf(2, second), spawnOf(3, third) }));
    h.events.then(check([&] {
        CHECK(h.budget.asked == 1);
        auto const& channel = h.launcher.channel(0);
        CHECK(channel.all<Spawned>().size() == 2);
        CHECK(channel.only<SpawnFailed>()
              == SpawnFailed { .request = 3, .error = SpawnError::TooManyDescriptors });
    }));
    // A new wait counts afresh.
    auto fourth = makePipe();
    h.events.then(deliver({ spawnOf(4, fourth) }));
    h.events.then(check([&] { CHECK(h.budget.asked == 2); }));
    h.run();
}

TEST_CASE("a shell is watched by the pidfd its spawn opened, and the daemon as the daemon", "[keeper]")
{
    auto h = Harness {};
    auto pidfd = makePipe();
    auto const handle = pidfd.give();
    h.spawner.processHandles.emplace_back(handle);
    auto child = makePipe();
    h.events.then(deliver({ spawnOf(7, child) }));
    h.events.then(check([&] {
        REQUIRE(h.events.watches.size() == 2);
        CHECK(h.events.watches[0].pid == FirstDaemon);
        CHECK(h.events.watches[0].role == WatchedRole::Daemon);
        CHECK(h.events.watches[0].processHandle == -1);
        CHECK(h.events.watches[1].pid == FirstShell);
        CHECK(h.events.watches[1].role == WatchedRole::Session);
        CHECK(h.events.watches[1].processHandle == handle);
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

TEST_CASE("a Spawn's executable descriptor comes last and is the spawner's to run by", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    auto executable = makePipe();
    auto spawn = Spawn { .request = 4,
                         .argv = { "contour-spawn" },
                         .executablePath = {},
                         .childFds = { 3 },
                         .kept = { KeptDescriptor::Master },
                         .executable = ExecutableSource::Descriptor };
    h.events.then(deliver({ frameOf(spawn, { child.give(), master.give(), executable.give() }) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<Spawned>().request == 4);
        REQUIRE(h.spawner.requests.size() == 1);
        CHECK(h.spawner.requests.front().descriptorCount == 1);
        CHECK(h.spawner.requests.front().executable.has_value());
        CHECK(!master.writerClosed());    // kept
        CHECK(executable.writerClosed()); // run by, then let go
    }));
    h.run();
}

TEST_CASE("a Spawn that names a descriptor executable but sends none is refused", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto spawn = Spawn { .request = 5,
                         .argv = { "contour-spawn" },
                         .executablePath = {},
                         .childFds = { 3 },
                         .kept = {},
                         .executable = ExecutableSource::Descriptor };
    h.events.then(deliver({ frameOf(spawn, { child.give() }) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<SpawnFailed>().error == SpawnError::Unknown);
        CHECK(h.spawner.requests.empty());
    }));
    h.run();
}

TEST_CASE("Signal reaches a held child, and never one reaped or released", "[keeper]")
{
    auto h = Harness {};
    auto first = makePipe();
    auto second = makePipe();
    h.events.then(deliver({ spawnOf(1, first), spawnOf(2, second) }));
    h.events.then(deliver({ frameOf(Signal { .session = 1, .signal = ChildSignal::Terminate }),
                            frameOf(Signal { .session = 2, .signal = ChildSignal::Kill }),
                            frameOf(Signal { .session = 99, .signal = ChildSignal::Kill }) }));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstShell) == std::vector { KeeperSignal::Terminate });
        CHECK(h.signaller.to(FirstShell + 1) == std::vector { KeeperSignal::Kill });
    }));
    // The first is reaped, the second released: neither is signalled again.
    h.events.then(deliver({ endOf(FirstShell, Killed),
                            frameOf(Release { .session = 2, .graceMs = 60'000 }),
                            frameOf(Signal { .session = 1, .signal = ChildSignal::Kill }),
                            frameOf(Signal { .session = 2, .signal = ChildSignal::Kill }) }));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstShell) == std::vector { KeeperSignal::Terminate });
        CHECK(h.signaller.to(FirstShell + 1) == std::vector { KeeperSignal::Kill });
    }));
    h.run();
}

TEST_CASE("a Signal read in the same wait as its shell's end reaches only the unreaped zombie", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(deliver({ spawnOf(1, child) }));
    // As the event source orders one wait: the frames first, then the ends.
    h.events.then(
        deliver({ frameOf(Signal { .session = 1, .signal = ChildSignal::Kill }), endOf(FirstShell) }));
    h.events.then(check([&] {
        // Sent while the zombie held the pid (FakeSignaller checks it was not reaped yet), then reaped.
        CHECK(h.signaller.to(FirstShell) == std::vector { KeeperSignal::Kill });
        CHECK(h.events.isReaped(FirstShell));
    }));
    h.events.then(deliver({ frameOf(Signal { .session = 1, .signal = ChildSignal::Kill }) }));
    h.events.then(check([&] { CHECK(h.signaller.to(FirstShell) == std::vector { KeeperSignal::Kill }); }));
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
              == Launch { .continuity = LaunchContinuity::Fresh,
                          .options = launchOptions(),
                          .originalDescriptorLimit = 1024 });
        auto const& second = h.launcher.channel(1);
        CHECK(std::holds_alternative<Hello>(second.sent.at(0).message));
        CHECK(std::get<Launch>(second.sent.at(1).message)
              == Launch { .continuity = LaunchContinuity::Recovered,
                          .options = launchOptions(),
                          .originalDescriptorLimit = 1024 });
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

namespace
{
/// The steps of a served first daemon that crashes, and of four restarts that crash on start, each after the
/// backoff the policy's default numbers give it: the fifth crash within five minutes gives up.
void crashFiveTimes(Harness& h)
{
    h.events.then(deliver(ended(FirstDaemon, Segfault)));     // restart at once
    h.events.then(deliver(ended(FirstDaemon + 1, Segfault))); // back off 1 s
    h.events.then(h.after(1s));
    h.events.then(deliver(ended(FirstDaemon + 2, Segfault))); // back off 2 s
    h.events.then(h.after(2s));
    h.events.then(deliver(ended(FirstDaemon + 3, Segfault))); // back off 4 s
    h.events.then(h.after(4s));
    h.events.then(deliver(ended(FirstDaemon + 4, Segfault))); // the fifth: give up
}
} // namespace

TEST_CASE("a daemon that crashes on every start gives up after five, and the shells live", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    h.events.then(
        deliver({ frameOf(Serving {}), spawnOf(1, child, { { KeptDescriptor::Master, &master } }) }));
    crashFiveTimes(h);
    h.events.then(h.after(119s)); // nearly the whole grace period
    h.events.then(check([&] {
        CHECK(h.launcher.images.size() == 5); // no sixth start: given up
        CHECK(!master.writerClosed());        // the shells are kept
        CHECK(h.signaller.to(FirstShell).empty());
        CHECK(h.socketFiles.removed.empty());
    }));
    h.events.then(h.after(1s));
    h.events.then(check([&] { CHECK(h.launcher.images.size() == 6); })); // the one retry
    h.run();
}

TEST_CASE("after the grace the keeper ends the shells, unlinks the socket and exits non-zero", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    auto unixListener = makePipe();
    auto tcpListener = makePipe();
    auto lockReleased = false;
    h.onLockReleased = [&] {
        lockReleased = true;
    };
    auto unlinkedUnderLock = false;
    h.socketFiles.onRemove = [&] {
        unlinkedUnderLock = !lockReleased;
    };
    h.events.then(deliver(
        { frameOf(Listener { .name = "native", .tlsIdentity = std::nullopt, .path = "/run/contour/default" },
                  { unixListener.give() }),
          frameOf(Listener { .name = "native-tcp", .tlsIdentity = std::nullopt, .path = {} },
                  { tcpListener.give() }),
          frameOf(Serving {}),
          spawnOf(1, child, { { KeptDescriptor::Master, &master } }) }));
    crashFiveTimes(h);
    h.events.then(h.after(120s));
    h.events.then(check(
        [&] {
            CHECK(h.socketFiles.removed.empty()); // kept for the retry
            CHECK(!unixListener.writerClosed());
        },
        ended(FirstDaemon + 5, Segfault))); // the retry crashes too: end everything
    h.events.then(check([&] {
        CHECK(h.socketFiles.removed == std::vector<std::filesystem::path> { "/run/contour/default" });
        CHECK(unlinkedUnderLock); // no other keeper can have bound the path yet
        CHECK(lockReleased);
        CHECK(unixListener.writerClosed());
        CHECK(tcpListener.writerClosed());
        CHECK(master.writerClosed()); // hung up
    }));
    h.events.then(h.after(3s));
    h.events.then(check([&] { CHECK(h.signaller.to(FirstShell) == std::vector { KeeperSignal::Kill }); },
                        { endOf(FirstShell, Killed) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::GaveUp);
    CHECK(h.exitStatus != 0);
    CHECK(h.launcher.images.size() == 6);
}

TEST_CASE("a newer image staged during the grace is the one retried", "[keeper]")
{
    auto const newer = std::filesystem::path { "/state/bin/0.9.0-0badcafe/contour" };
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}) }));
    crashFiveTimes(h);
    h.events.then(check([&] {
        // Every restart so far asked for the newest image no older than the committed one, and got it.
        CHECK(h.images.picks == std::vector<std::filesystem::path>(4, daemonImage()));
        CHECK(h.launcher.images == std::vector<std::filesystem::path>(5, daemonImage()));
    }));
    h.events.then(h.after(60s));
    h.events.then(check([&] { h.images.newer = newer; })); // staged half way through the grace period
    h.events.then(h.after(60s));
    h.events.then(check([&] {
        REQUIRE(h.launcher.images.size() == 6);
        CHECK(h.launcher.images.back() == newer);
        CHECK(h.launcher.channel(5).only<Launch>().continuity == LaunchContinuity::Recovered);
    }));
    h.run();
}

TEST_CASE("a retry that stays up five minutes clears the give-up, and its crash is restarted", "[keeper]")
{
    // No heartbeats come from a fake daemon: the five minutes must not count as a hang.
    auto const unwatched = Serving { .hungRestart = HungRestart::Off, .heartbeatTimeoutMs = 30'000 };
    auto h = Harness {};
    h.events.then(deliver({ frameOf(unwatched) }));
    crashFiveTimes(h);
    h.events.then(h.after(120s));
    h.events.then(check([&] { CHECK(h.launcher.images.size() == 6); }, { frameOf(unwatched) })); // it serves
    h.events.then(h.after(299s));
    h.events.then(deliver(ended(FirstDaemon + 5, Segfault))); // not yet stable: ends everything
    h.run();
    CHECK(h.outcome == KeeperOutcome::GaveUp);
    CHECK(h.launcher.images.size() == 6);

    auto stable = Harness {};
    stable.events.then(deliver({ frameOf(unwatched) }));
    crashFiveTimes(stable);
    stable.events.then(stable.after(120s));
    stable.events.then(check([&] { CHECK(stable.launcher.images.size() == 6); }, { frameOf(unwatched) }));
    stable.events.then(stable.after(300s));
    stable.events.then(deliver(ended(FirstDaemon + 5, Segfault))); // stable by now: a first crash again
    stable.events.then(check([&] {
        REQUIRE(stable.launcher.images.size() == 7); // restarted at once, not given up on
        CHECK(stable.launcher.channel(6).only<Launch>().continuity == LaunchContinuity::Recovered);
    }));
    stable.run();
    CHECK(stable.outcome == KeeperOutcome::Clean);
}

TEST_CASE("a restart launch that keeps failing gives up without recursing", "[keeper]")
{
    auto h = Harness {};
    // Every crash restarts at once, so every failed launch is followed by another one at once.
    h.launcher.failingAttempts = { 1, 2, 3, 4, 5 };
    h.events.then(deliver({ frameOf(Serving {}) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(check([&] {
        CHECK(h.launcher.images.size() == 5); // the served one, then four failed: five crashes
        CHECK(h.launcher.channels.size() == 1);
    }));
    h.events.then(h.after(120s)); // the retry fails too: everything ends
    h.run(RestartPolicyConfig { .quickRepeat = 0ms });
    CHECK(h.outcome == KeeperOutcome::GaveUp);
    CHECK(h.launcher.images.size() == 6);
}

TEST_CASE("a daemon that ignores a forwarded stop is killed after the bound", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    h.events.then(deliver({ frameOf(Serving {}),
                            spawnOf(1, child, { { KeptDescriptor::Master, &master } }),
                            eventOf(EventKind::StopSignal) }));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Terminate });
        CHECK(h.events.lastDeadline == h.clock.now() + 5s);
    }));
    h.events.then(h.after(4s));
    h.events.then(check([&] {
        CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Terminate });
        CHECK(!master.writerClosed()); // the shells wait for the daemon's end
    }));
    h.events.then(h.after(1s));
    h.events.then(check(
        [&] {
            CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Terminate, KeeperSignal::Kill });
        },
        ended(FirstDaemon, Killed)));
    h.events.then(check([&] {
        CHECK(master.writerClosed());         // hung up
        CHECK(h.launcher.images.size() == 1); // a stopped daemon is never restarted
    }));
    h.events.then(h.after(3s));
    h.events.then(check([&] { CHECK(h.signaller.to(FirstShell) == std::vector { KeeperSignal::Kill }); },
                        { endOf(FirstShell, Killed) }));
    h.run(RestartPolicyConfig { .daemonExitBound = 5s });
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.exitStatus == 0);
}

TEST_CASE("the stop bound defaults to a minute, below systemd's stop timeout", "[keeper]")
{
    CHECK(RestartPolicyConfig {}.daemonExitBound == 60s);
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Serving {}), eventOf(EventKind::StopSignal) }));
    h.events.then(h.after(59s));
    h.events.then(
        check([&] { CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Terminate }); }));
    h.events.then(h.after(1s));
    h.events.then(check(
        [&] {
            CHECK(h.signaller.to(FirstDaemon) == std::vector { KeeperSignal::Terminate, KeeperSignal::Kill });
        },
        ended(FirstDaemon, Killed)));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.exitStatus == 0);
}

TEST_CASE("Shutdown is noted once the label and the listeners are let go", "[keeper]")
{
    auto h = Harness {};
    auto listener = makePipe();
    auto lockReleased = false;
    auto lockReleasedBeforeNoted = false;
    h.onLockReleased = [&] {
        lockReleased = true;
        lockReleasedBeforeNoted = h.launcher.channel(0).all<ShutdownNoted>().empty();
    };
    auto unlinkedUnderLock = false;
    h.socketFiles.onRemove = [&] {
        unlinkedUnderLock = !lockReleased;
    };
    h.events.then(deliver(
        { frameOf(Listener { .name = "native", .tlsIdentity = std::nullopt, .path = "/run/contour/default" },
                  { listener.give() }),
          frameOf(Serving {}) }));
    h.events.then(deliver({ frameOf(Shutdown { .cause = 3 }) }));
    h.events.then(check(
        [&] {
            auto const& sent = h.launcher.channel(0).sent;
            REQUIRE(!sent.empty());
            CHECK(std::holds_alternative<ShutdownNoted>(sent.back().message));
            CHECK(lockReleased);
            CHECK(lockReleasedBeforeNoted); // the daemon closes its listeners on the note: no label held then
            CHECK(listener.writerClosed());
            // A daemon behind a keeper removes no socket file: the keeper does, before anyone may bind it
            // anew.
            CHECK(h.socketFiles.removed == std::vector<std::filesystem::path> { "/run/contour/default" });
            CHECK(unlinkedUnderLock);
        },
        ended(FirstDaemon)));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.exitStatus == 0);
}

TEST_CASE("a stop during the grace period ends the shells and exits zero", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    auto unixListener = makePipe();
    h.events.then(deliver(
        { frameOf(Listener { .name = "native", .tlsIdentity = std::nullopt, .path = "/run/contour/default" },
                  { unixListener.give() }),
          frameOf(Serving {}),
          spawnOf(1, child, { { KeptDescriptor::Master, &master } }) }));
    crashFiveTimes(h);
    h.events.then(check([&] {
        CHECK(h.socketFiles.removed.empty()); // kept through the grace period
        CHECK(!unixListener.writerClosed());
    }));
    h.events.then(deliver({ eventOf(EventKind::StopSignal) }));
    h.events.then(check([&] {
        CHECK(master.writerClosed());
        // No daemon is left to remove the socket file the crashed one bound: the keeper does, as at any end.
        CHECK(h.socketFiles.removed == std::vector<std::filesystem::path> { "/run/contour/default" });
        CHECK(unixListener.writerClosed());
    }));
    h.events.then(h.after(3s));
    h.events.then(check([] {}, { endOf(FirstShell, Killed) }));
    h.run();
    CHECK(h.outcome == KeeperOutcome::Clean);
    CHECK(h.exitStatus == 0);
    CHECK(h.launcher.images.size() == 5);
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
              == Launch { .continuity = LaunchContinuity::HandedOver,
                          .options = launchOptions(),
                          .originalDescriptorLimit = 1024 });
        CHECK(h.launcher.channel(0).all<Hello>().size() == 2);
        CHECK(h.launcher.images.size() == 1); // the same process: nothing was launched
        CHECK(h.signaller.sent.empty());
    }));
    h.run();
}

TEST_CASE("a new image of a live upgrade is told again of every held session that ended", "[keeper]")
{
    auto h = Harness {};
    auto child = makePipe();
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}), spawnOf(1, child) }));
    h.events.then(deliver({ endOf(FirstShell, Killed) }));
    h.events.then(check([&] { CHECK(h.launcher.channel(0).all<Exited>().size() == 1); }));
    // The exec in place: the new image says Hello on the same channel.
    h.events.then(deliver({ frameOf(Hello {}) }));
    h.events.then(check([&] {
        auto const& sent = h.launcher.channel(0).sent;
        auto const exits = h.launcher.channel(0).all<Exited>();
        REQUIRE(exits.size() == 2);
        CHECK(std::get<Exited>(exits.back()->message) == Exited { .session = 1, .status = Killed });
        // After its Launch, which it reads first.
        CHECK(std::holds_alternative<Launch>(sent[sent.size() - 2].message));
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
    h.events.then(check([&] {
        CHECK(first.writerClosed());
        // The same socket deposited again: its file is still the one served, and stays.
        CHECK(h.socketFiles.removed.empty());
    }));
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

TEST_CASE("after an upgrade a crash restarts the new image", "[keeper][upgrade]")
{
    auto const upgraded = std::filesystem::path { "/state/bin/0.9.0-bbbbbbbb/contour" };
    auto h = Harness {};
    h.images.images = { upgraded };
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}) }));
    // The daemon is about to exec the new image, which then says Hello on the same channel.
    h.events.then(deliver({ frameOf(Exec { .executablePath = upgraded.string() }) }));
    h.events.then(
        check([&] { CHECK(*h.images.heldCount == 1); }, { frameOf(Hello {}), frameOf(Serving {}) }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(check([&] {
        CHECK(h.launcher.images == std::vector { daemonImage(), upgraded });
        CHECK(h.images.picks == std::vector { upgraded }); // nothing older than it is ever picked
        CHECK(*h.images.heldCount == 1);                   // held for as long as it would be restarted
    }));
    h.run();
    CHECK(*h.images.heldCount == 0);
}

TEST_CASE("an exec that never happened leaves the keeper restarting the image it ran", "[keeper][upgrade]")
{
    auto const upgraded = std::filesystem::path { "/state/bin/0.9.0-bbbbbbbb/contour" };
    auto h = Harness {};
    h.images.images = { upgraded };
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}) }));
    // The exec failed: the old image serves on, and never says Hello again.
    h.events.then(deliver({ frameOf(Exec { .executablePath = upgraded.string() }), frameOf(Heartbeat {}) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).all<Launch>().size() == 1);
        CHECK(h.signaller.sent.empty());
    }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(check([&] {
        CHECK(h.launcher.images == std::vector { daemonImage(), daemonImage() });
        CHECK(*h.images.heldCount == 0); // the announced image is let go with the process that announced it
    }));
    h.run();
}

TEST_CASE("an Exec of anything but an image is refused, and the image the keeper ran is kept",
          "[keeper][upgrade]")
{
    auto h = Harness {};
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}) }));
    h.events.then(deliver({ frameOf(Exec { .executablePath = "/tmp/evil" }), frameOf(Hello {}) }));
    h.events.then(check([&] {
        // The upgrade itself goes on: the new image is launched as handed over all the same.
        auto const launches = h.launcher.channel(0).all<Launch>();
        REQUIRE(launches.size() == 2);
        CHECK(std::get<Launch>(launches.back()->message).continuity == LaunchContinuity::HandedOver);
    }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(check([&] { CHECK(h.launcher.images == std::vector { daemonImage(), daemonImage() }); }));
    h.run();
}

TEST_CASE("frames for a daemon that execs are held back until its new image says Hello", "[keeper][upgrade]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto second = makePipe();
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}), spawnOf(1, child) }));
    h.events.then(deliver({ frameOf(Exec { .executablePath = "/state/bin/0.9.0-bbbbbbbb/contour" }) }));
    // The shell ends while the daemon execs; a spawn it asks for meanwhile is refused.
    h.events.then(deliver({ endOf(FirstShell, Killed), spawnOf(2, second) }));
    h.events.then(check([&] {
        auto const& sent = h.launcher.channel(0).sent;
        // ExecNoted is the last thing the image that execs reads: nothing after it until the new image.
        REQUIRE_FALSE(sent.empty());
        CHECK(std::holds_alternative<ExecNoted>(sent.back().message));
        CHECK(h.launcher.channel(0).all<Exited>().empty());
        CHECK(h.spawner.requests.size() == 1);
    }));
    h.events.then(deliver({ frameOf(Hello {}) }));
    h.events.then(check([&] {
        auto const& sent = h.launcher.channel(0).sent;
        auto const launch = std::ranges::find_if(sent, [](auto const& entry) {
            return std::holds_alternative<Launch>(entry.message)
                   && std::get<Launch>(entry.message).continuity == LaunchContinuity::HandedOver;
        });
        REQUIRE(launch != sent.end());
        // After the new image's Launch, in order, what was held back -- the exit as it happened and the
        // refused spawn's answer. The exit is told once: the Hello's retelling leaves out an exit held back.
        auto const kinds = std::ranges::to<std::vector>(
            std::ranges::subrange(std::next(launch), sent.end())
            | std::views::transform([](auto const& entry) { return entry.message.index(); }));
        CHECK(kinds
              == std::vector<std::size_t> { Message { Exited {} }.index(),
                                            Message { SpawnFailed {} }.index() });
        CHECK(h.launcher.channel(0).only<Exited>() == Exited { .session = 1, .status = Killed });
        CHECK(std::get<SpawnFailed>(sent.back().message)
              == SpawnFailed { .request = 2, .error = SpawnError::ShuttingDown });
    }));
    h.run();
}

TEST_CASE("an aborted exec releases what was held back and forgets the image", "[keeper][upgrade]")
{
    auto const upgraded = std::filesystem::path { "/state/bin/0.9.0-bbbbbbbb/contour" };
    auto h = Harness {};
    h.images.images = { upgraded };
    auto child = makePipe();
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}), spawnOf(1, child) }));
    h.events.then(
        deliver({ frameOf(Exec { .executablePath = upgraded.string() }), endOf(FirstShell, Killed) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).all<Exited>().empty());
        CHECK(*h.images.heldCount == 1);
    }));
    h.events.then(deliver({ frameOf(ExecAborted {}) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).only<Exited>() == Exited { .session = 1, .status = Killed });
        CHECK(*h.images.heldCount == 0);
    }));
    // The old image serves on; a Hello it never sends cannot commit the image, so a crash restarts its own.
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(check([&] { CHECK(h.launcher.images == std::vector { daemonImage(), daemonImage() }); }));
    h.run();
}

TEST_CASE("an Exec refused after an accepted one leaves nothing for the Hello to commit", "[keeper][upgrade]")
{
    auto const upgraded = std::filesystem::path { "/state/bin/0.9.0-bbbbbbbb/contour" };
    auto h = Harness {};
    h.images.images = { upgraded };
    h.events.then(deliver({ frameOf(Hello {}), frameOf(Serving {}) }));
    h.events.then(deliver({ frameOf(Exec { .executablePath = upgraded.string() }) }));
    h.events.then(check([&] { CHECK(*h.images.heldCount == 1); }));
    // Another Exec names something that is no image: the first target must not survive it.
    h.events.then(deliver({ frameOf(Exec { .executablePath = "/tmp/evil" }), frameOf(Hello {}) }));
    h.events.then(check([&] { CHECK(*h.images.heldCount == 0); }));
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(check([&] { CHECK(h.launcher.images == std::vector { daemonImage(), daemonImage() }); }));
    h.run();
}

TEST_CASE("a daemon that crashes while frames are held back leaves none of them to its successor",
          "[keeper][upgrade]")
{
    auto h = Harness {};
    auto child = makePipe();
    auto master = makePipe();
    h.events.then(deliver({ frameOf(Hello {}),
                            frameOf(Serving {}),
                            spawnOf(1, child, { { KeptDescriptor::Master, &master } }) }));
    h.events.then(deliver({ frameOf(Exec { .executablePath = "/state/bin/0.9.0-bbbbbbbb/contour" }),
                            endOf(FirstShell, Killed) }));
    // The exec never finished: the process died before its new image said Hello.
    h.events.then(deliver(ended(FirstDaemon, Segfault)));
    h.events.then(deliver({ frameOf(Adopt {}) }));
    h.events.then(check([&] {
        CHECK(h.launcher.channel(0).all<Exited>().empty());
        auto const& recovered = h.launcher.channel(1);
        // Nothing held back for the dead daemon reaches its successor; the exit is in the inventory.
        CHECK(recovered.all<Exited>().empty());
        CHECK(recovered.all<ExecNoted>().empty());
        CHECK(recovered.only<InventorySession>().exited == Killed);
        CHECK(std::get<Launch>(recovered.sent.at(1).message).continuity == LaunchContinuity::Recovered);
    }));
    h.run();
}
