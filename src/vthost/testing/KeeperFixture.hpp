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
};

} // namespace vthost::testing

template <>
struct core::codec::EnumWire<vthost::testing::HarnessStart>
{
    static constexpr auto Values = std::array {
        vthost::testing::HarnessStart::Serves,
        vthost::testing::HarnessStart::Fails,
        vthost::testing::HarnessStart::Relays,
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

/// What the harness keeper hands its daemon, as the options record: opaque to the keeper, as `contour
/// daemon`'s DaemonLaunch is.
struct HarnessLaunch
{
    std::string
        socketPath;        ///< The daemon's control socket; for HarnessStart::Relays, where the test listens.
    std::string launchLog; ///< The file each daemon process appends its pid to, as it starts.
    HarnessStart start = HarnessStart::Serves;

    /// Lists the fields.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.socketPath);
        visitor(2, self.launchLog);
        visitor(3, self.start);
    }
};

/// @param pid A process.
/// @return Its parent, as /proc/<pid>/stat states it; 0 when that cannot be read.
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

    /// Waits for the keeper to end, and reaps it.
    /// @param bound How long to wait.
    /// @return Its exit status; nullopt when it is still running, or ended by a signal.
    [[nodiscard]] std::optional<int> awaitKeeperExit(std::chrono::milliseconds bound);

    /// Starts another keeper on this fixture's socket, as a second `contour daemon` on the same label would.
    /// @return Its exit status; nullopt when it did not end within @p bound (it is killed then).
    [[nodiscard]] std::optional<int> runSecondKeeper(std::chrono::milliseconds bound) const;

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

    [[nodiscard]] int spawnKeeper(HarnessStart start) const;

    std::filesystem::path _directory;
    std::filesystem::path _socket;
    std::filesystem::path _launchLog;
    std::filesystem::path _output;
    /// Whether the keeper process was reaped already.
    enum class KeeperState : uint8_t
    {
        Running = 0,
        Reaped,
    };

    keeper::UniqueFd _relayListener; ///< For HarnessStart::Relays: where the daemon hands its channel over.
    keeper::UniqueFd _relay;         ///< Its connection: closing it lets the daemon end.
    keeper::UniqueFd _channel;       ///< The channel it handed over, until taken.
    int _keeper = 0;
    KeeperState _keeperState = KeeperState::Running;

    /// For HarnessStart::Relays: accepts the daemon's connection and receives its channel.
    void acceptRelay();
};

} // namespace vthost::testing
