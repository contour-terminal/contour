// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/keeper/Ports.hpp>
#include <vthost/keeper/RestartPolicy.hpp>

/// The keeper as a process: what `contour daemon` (and the test harness) runs once it has decided what to
/// keep.
namespace vthost::keeper
{

/// Why a keeper process did not start keeping.
enum class KeeperStartError : uint8_t
{
    MultiThreaded = 0, ///< Another thread runs: the signal mask and PR_SET_PDEATHSIG would not hold.
    LabelHeld,         ///< Another keeper holds the label; no daemon was started.
    LabelUnlockable,   ///< The label's lock file could not be opened.
    PortsUnavailable,  ///< The signal watch could not be set up.
};

/// @param error Why the keeper did not start.
/// @return The reason, as a sentence fragment for the log and the console.
[[nodiscard]] std::string_view describe(KeeperStartError error) noexcept;

/// What a keeper process keeps, fixed for its life.
struct KeeperProcessConfig
{
    std::filesystem::path daemonImage;        ///< The daemon executable.
    std::vector<std::string> daemonArguments; ///< Its argv after argv[0]: frozen (@see KeeperPortsConfig).
    std::vector<std::string> environment;     ///< The daemon's and every shell's environment, `NAME=value`.
    std::filesystem::path socketPath;         ///< The label's socket, whose lock (labelLockPath()) is taken.
    std::vector<uint8_t> launchOptions;       ///< The options record, opaque here; sent with every Launch.
    RestartPolicyConfig policy {};            ///< The crash-loop policy's numbers.
    /// What a crashed daemon is restarted from, and the lease on it; null for the daemon image, always.
    std::unique_ptr<RestartImages> images = nullptr;
};

/// @return This process's environment, each variable as `NAME=value`: what a keeper hands its daemon and
///         every shell it starts.
[[nodiscard]] std::vector<std::string> processEnvironmentEntries();

/// @return How many threads this process runs, where the platform says (Linux: `/proc/self/status`);
///         nullopt where it cannot be told.
[[nodiscard]] std::optional<std::size_t> runningThreadCount();

/// Keeps a daemon: asserts that the process is single-threaded, takes the label's lock, builds the POSIX
/// ports and runs the Keeper until it ends. Blocks.
///
/// Called before anything of the process starts a thread: the ports block the stop signals and SIGCHLD in
/// the calling thread alone, and PR_SET_PDEATHSIG binds a child to the thread that forked it. A second
/// thread would break both silently, so it fails the start, loudly, instead.
/// @param config What to keep.
/// @return The status the keeper process exits with (@see Keeper::exitStatus); or why it did not start, in
///         which case no daemon was started.
[[nodiscard]] std::expected<int, KeeperStartError> runKeeperProcess(KeeperProcessConfig config);

} // namespace vthost::keeper
