// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A daemon behind a real PTY keeper, as `contour daemon` runs it, out of the test binary itself:
/// `vthost_test --keeper` is the keeper, `vthost_test --keeper-serve` the daemon it starts. POSIX only.

#include <core/codec/Fields.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/keeper/UniqueFd.hpp>

namespace vthost::testing
{

/// Whether the harness daemon starts at all.
enum class HarnessStart : uint8_t
{
    Serves = 0, ///< It serves, as a configured daemon does.
    Fails,      ///< It ends before serving, as a daemon with an invalid configuration does.
    /// It hands its channel to the test (KeeperFixture::takeChannel()), which plays the daemon from then on,
    /// and lives until the test lets it go: the keeper's daemon in name, so that the test's own process
    /// spawns through the keeper as a daemon would.
    Relays,
    /// The first daemon relays, as Relays; every daemon the keeper starts after it serves, as Serves: what a
    /// test that plays a daemon to its crash, and then watches the real one recover, starts.
    RelaysThenServes,
    /// The first daemon serves, as Serves; every daemon the keeper restarts after it is killed by a signal
    /// before it serves: a crash loop.
    ServesThenCrashes,
    /// It serves, as Serves, but ignores SIGTERM, SIGINT and SIGHUP: a daemon a forwarded stop does not end.
    IgnoresStop,
};

/// When the harness daemon ends of its own accord.
enum class HarnessLifecycle : uint8_t
{
    Persistent = 0,
    ExitWhenIdle, ///< After a second with no session and no client.
};

/// Whether the harness daemon keeps a workspace snapshot.
enum class HarnessSnapshot : uint8_t
{
    None = 0,
    Kept, ///< At KeeperFixture::snapshotPath(), written every second.
};

/// Whether the harness daemon serves the native protocol over TCP too.
enum class HarnessTcp : uint8_t
{
    Off = 0,
    Loopback, ///< On 127.0.0.1, on a port the system picks, with a self-signed certificate.
};

} // namespace vthost::testing

template <>
struct core::codec::EnumWire<vthost::testing::HarnessStart>
{
    static constexpr auto Values = std::array {
        vthost::testing::HarnessStart::Serves,
        vthost::testing::HarnessStart::Fails,
        vthost::testing::HarnessStart::Relays,
        vthost::testing::HarnessStart::RelaysThenServes,
        vthost::testing::HarnessStart::ServesThenCrashes,
        vthost::testing::HarnessStart::IgnoresStop,
    };
};

template <>
struct core::codec::EnumWire<vthost::testing::HarnessLifecycle>
{
    static constexpr auto Values = std::array {
        vthost::testing::HarnessLifecycle::Persistent,
        vthost::testing::HarnessLifecycle::ExitWhenIdle,
    };
};

template <>
struct core::codec::EnumWire<vthost::testing::HarnessSnapshot>
{
    static constexpr auto Values = std::array {
        vthost::testing::HarnessSnapshot::None,
        vthost::testing::HarnessSnapshot::Kept,
    };
};

template <>
struct core::codec::EnumWire<vthost::testing::HarnessTcp>
{
    static constexpr auto Values = std::array {
        vthost::testing::HarnessTcp::Off,
        vthost::testing::HarnessTcp::Loopback,
    };
};

namespace vthost::testing
{

/// The status the harness keeper exits with when it could not start keeping (another keeper holds the label,
/// among the reasons).
inline constexpr int HarnessKeeperStartFailureStatus = 70;

/// A variable the harness keeper adds to the environment it runs its children under (its daemon and every
/// spawn trampoline), and that the test process never has: a program a keeper spawns must not see it.
inline constexpr auto KeeperOnlyVariable = std::string_view { "CONTOUR_KEEPER_HARNESS_ONLY" };

/// The status the harness daemon exits with when told to fail its start.
inline constexpr int HarnessStartFailureStatus = 3;

/// The harness keeper's crash-loop and stop numbers, in milliseconds; 0 keeps the keeper's default.
struct HarnessTimings
{
    uint64_t backoffMs = 0;         ///< The first backoff, and its cap: every backoff is this long.
    uint64_t graceMs = 0;           ///< How long a keeper that gave up waits before its one retry.
    uint64_t daemonExitBoundMs = 0; ///< How long a stopped daemon has to exit before it is killed.
};

/// What the harness keeper hands its daemon, as the options record: opaque to the keeper, as `contour
/// daemon`'s DaemonLaunch is.
struct HarnessLaunch
{
    std::string socketPath; ///< The daemon's control socket.
    std::string launchLog;  ///< The file each daemon process appends its pid to, as it starts.
    HarnessStart start = HarnessStart::Serves;
    /// The startup layout: a tab per command, each run as `/bin/sh -c <command>`.
    std::vector<std::string> panes;
    /// The workspace snapshot, written every second; empty for none.
    std::string snapshotPath;
    HarnessTcp tcp = HarnessTcp::Off;
    /// Where a relaying daemon hands its channel over.
    std::string relayPath;
    HarnessTimings timings {};
    /// How often the snapshot is written, in milliseconds; 0 for every second.
    uint64_t snapshotIntervalMs = 0;
    HarnessLifecycle lifecycle = HarnessLifecycle::Persistent;

    /// Lists the fields.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.socketPath);
        visitor(2, self.launchLog);
        visitor(3, self.start);
        visitor(4, self.panes);
        visitor(5, self.snapshotPath);
        visitor(6, self.tcp);
        visitor(7, self.relayPath);
        visitor(8, self.timings.backoffMs);
        visitor(9, self.timings.graceMs);
        visitor(10, self.timings.daemonExitBoundMs);
        visitor(11, self.snapshotIntervalMs);
        visitor(12, self.lifecycle);
    }
};

/// What a KeeperFixture's daemon runs beside how it starts.
struct HarnessSetup
{
    HarnessStart start = HarnessStart::Serves;
    /// As HarnessLaunch::panes; `@DIR@` in a command stands for KeeperFixture::directory().
    std::vector<std::string> panes;
    HarnessSnapshot snapshot = HarnessSnapshot::None;
    HarnessTcp tcp = HarnessTcp::Off;
    /// The soft descriptor limit the keeper is started under, as a user's session may have it; nullopt keeps
    /// the test's own.
    std::optional<uint64_t> softDescriptorLimit = std::nullopt;
    HarnessTimings timings {}; ///< The keeper's crash-loop and stop numbers.
    /// As HarnessLaunch::snapshotIntervalMs.
    uint64_t snapshotIntervalMs = 0;
    HarnessLifecycle lifecycle = HarnessLifecycle::Persistent;
};

/// @param pid A process.
/// @return Its parent, as /proc/<pid>/stat (Linux) or proc_pidinfo() (macOS) states it; 0 when that cannot be
///         read.
[[nodiscard]] int parentProcessOf(int pid);

/// Runs the keeper or its daemon when the test binary is invoked as one. Called first thing in main(),
/// before anything starts a thread.
/// @param argc As main() got it.
/// @param argv As main() got it.
/// @return The status the process exits with, when it ran as one; nullopt for a test run.
[[nodiscard]] std::optional<int> runKeeperHarnessIfRequested(int argc, char const* argv[]);

/// A keeper process with its daemon, on a socket of its own; ended (SIGTERM, then SIGKILL) and reaped with
/// this object.
class KeeperFixture
{
  public:
    /// Starts the keeper and, for HarnessStart::Serves, waits until its daemon serves; for
    /// HarnessStart::Relays, until its daemon handed over its channel.
    /// @param start Whether the daemon serves, fails, or relays.
    explicit KeeperFixture(HarnessStart start = HarnessStart::Serves);

    /// Starts the keeper as KeeperFixture(HarnessStart) does, its daemon set up as @p setup says.
    /// @param setup What the daemon runs.
    explicit KeeperFixture(HarnessSetup const& setup);
    KeeperFixture(KeeperFixture const&) = delete;
    KeeperFixture& operator=(KeeperFixture const&) = delete;
    KeeperFixture(KeeperFixture&&) = delete;
    KeeperFixture& operator=(KeeperFixture&&) = delete;
    ~KeeperFixture();

    /// @return The keeper process.
    [[nodiscard]] int keeperPid() const noexcept { return _keeper; }

    /// @return The daemon process, as its status states it; 0 when it does not answer.
    [[nodiscard]] int daemonPid() const;

    /// Sends @p signum to the daemon.
    void killDaemon(int signum) const;

    /// @return A control client of the daemon.
    [[nodiscard]] std::unique_ptr<client::DaemonControlPort> client() const;

    /// For HarnessStart::Relays: the keeper's channel, past Hello and Launch, for the test to play the daemon
    /// on. It says Serving first, or the keeper takes its daemon for one that never started; and Shutdown
    /// before it closes the channel, or the keeper takes it for a crash.
    /// @return The channel, once; none after.
    [[nodiscard]] keeper::UniqueFd takeChannel() noexcept { return std::move(_channel); }

    /// @return The daemon's control socket.
    [[nodiscard]] std::filesystem::path const& socketPath() const noexcept { return _socket; }

    /// @return The pid of every daemon process started so far, in order.
    [[nodiscard]] std::vector<int> launches() const;

    /// Waits until a daemon other than @p previous serves.
    /// @param previous The daemon that served before.
    /// @param bound How long to wait.
    /// @return The new daemon's pid; 0 when none served in time.
    [[nodiscard]] int awaitNewDaemon(int previous, std::chrono::milliseconds bound) const;

    /// @return The directory the fixture keeps its files in, removed with it.
    [[nodiscard]] std::filesystem::path const& directory() const noexcept { return _directory; }

    /// @return The daemon's workspace snapshot, for HarnessSetup::keepsSnapshot.
    [[nodiscard]] std::filesystem::path snapshotPath() const { return _directory / "workspace.snapshot"; }

    /// @return The port the daemon's native TCP listener is on, as its log names it; 0 for none yet.
    [[nodiscard]] uint16_t nativeTcpPort() const;

    /// Waits for the keeper to end, and reaps it.
    /// @param bound How long to wait.
    /// @return Its exit status; nullopt when it is still running, or ended by a signal.
    [[nodiscard]] std::optional<int> awaitKeeperExit(std::chrono::milliseconds bound);

    /// Starts another keeper on this fixture's socket, as a second `contour daemon` on the same label would.
    /// @return Its exit status; nullopt when it did not end within @p bound (it is killed then).
    [[nodiscard]] std::optional<int> runSecondKeeper(std::chrono::milliseconds bound) const;

    /// Starts another keeper on this fixture's socket, as `contour daemon` run again would, and leaves it
    /// running: the fixture ends it (SIGTERM, then SIGKILL) and reaps it with itself.
    /// @return Its pid.
    int startSecondKeeper();

    /// @return Whether the keeper startSecondKeeper() started still runs (it has not exited, unreaped).
    [[nodiscard]] bool secondKeeperRuns() const;

    /// @return What the keeper and its daemons wrote to their standard error.
    [[nodiscard]] std::string log() const;

    /// How long the constructor waits for the daemon to serve: generous, for debug and sanitizer builds on a
    /// loaded machine; a daemon that serves returns at once.
    static constexpr auto StartBound = std::chrono::seconds { 60 };

  private:
    /// How end() ends the processes.
    enum class Ending : uint8_t
    {
        Orderly = 0, ///< SIGTERM to the keeper, which stops its daemon; SIGKILL after 15 s.
        AtOnce,      ///< SIGKILL to the daemon and the keeper.
    };

    /// Ends what the fixture started and reaps the keeper, by recorded pid, and removes the directory.
    void end(Ending ending);

    [[nodiscard]] int spawnKeeper() const;

    std::filesystem::path _directory;
    std::filesystem::path _socket;
    std::filesystem::path _launchLog;
    std::filesystem::path _output;
    std::filesystem::path _record; ///< The encoded HarnessLaunch the keeper is started with.
    /// Whether the keeper process was reaped already.
    enum class KeeperState : uint8_t
    {
        Running = 0,
        Reaped,
    };

    std::optional<uint64_t> _softDescriptorLimit; ///< @see HarnessSetup::softDescriptorLimit
    keeper::UniqueFd _relayListener; ///< For HarnessStart::Relays: where the daemon hands its channel over.
    keeper::UniqueFd _relay;         ///< Its connection: closing it lets the daemon end.
    keeper::UniqueFd _channel;       ///< The channel it handed over, until taken.
    int _keeper = 0;
    KeeperState _keeperState = KeeperState::Running;
    int _secondKeeper = 0; ///< startSecondKeeper()'s, until end() reaped it; 0 for none.

    /// For HarnessStart::Relays: accepts the daemon's connection and receives its channel.
    void acceptRelay();
};

} // namespace vthost::testing
