// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Windows handoff of a live upgrade. Windows has no exec that keeps the process, so the old daemon
/// starts the new release as a successor process that inherits exactly the handles the image names, and the
/// two meet at one commit point:
///
///  1. the successor reads the image and, able to take over, sets `ready`; it has touched no PTY and no
///     listener yet;
///  2. the old daemon, seeing it ready and still running, sets `commit` and ends at once (no destructor runs,
///     so nothing it owned is closed);
///  3. the successor, seeing `commit` -- or the old daemon gone without it, which can then resume nothing --
///     waits for the old daemon to have ended, and only then serves: replays the input in flight, reads and
///     writes the PTYs, accepts on the listeners.
///
/// A successor that ends, or is not ready in time, before step 2 is ended (if need be) and the old daemon
/// resumes every session and listener, as after a failed exec: it wrote nothing anywhere that the old daemon
/// would then deliver a second time.

#include <vtpty/CrossingHandle.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <vthost/handover/HandoverToken.hpp>
#include <vthost/handover/ImageHandoff.hpp>

namespace vthost::handover
{

class HandoverMemory;

#ifdef _WIN32
/// A successor process SuccessorLauncher started.
struct SuccessorProcess
{
    vtpty::CrossingHandle process {}; ///< A handle to it, which the caller closes.
    std::int64_t pid = -1;            ///< Its process id.
};

/// What the Windows handoff reaches out of the process with: starting the successor, ending it, and ending
/// this process. An interface, so that the handoff is tested without starting or ending anything.
class SuccessorLauncher
{
  public:
    SuccessorLauncher() = default;
    SuccessorLauncher(SuccessorLauncher const&) = delete;
    SuccessorLauncher(SuccessorLauncher&&) = delete;
    SuccessorLauncher& operator=(SuccessorLauncher const&) = delete;
    SuccessorLauncher& operator=(SuccessorLauncher&&) = delete;
    virtual ~SuccessorLauncher() = default;

    /// Starts @p executable with @p argv, detached from any console, inheriting exactly @p inherited, and
    /// this process's standard output and error where they are no console.
    /// @param executable The program.
    /// @param argv Its command line, the program first.
    /// @param inherited The handles it inherits, under the same values. None stays inheritable by a process
    ///        this one starts later.
    /// @return The process; or why it could not be started.
    [[nodiscard]] virtual std::expected<SuccessorProcess, std::error_code> launch(
        std::filesystem::path const& executable,
        std::span<std::string const> argv,
        std::span<vtpty::CrossingHandle const> inherited) = 0;

    /// Ends @p successor, one that failed to take over, and waits until it has ended: what lets the old
    /// daemon resume knowing the successor touches nothing more.
    /// @param successor What launch() started.
    virtual void terminate(SuccessorProcess const& successor) = 0;

    /// Ends this process at once, the successor having taken over: no destructor and no handler at exit runs,
    /// that could close what the successor owns now -- a pseudoconsole closed here would end its program.
    [[noreturn]] virtual void endThisProcess() = 0;
};

/// @return The operating system's launcher.
[[nodiscard]] std::unique_ptr<SuccessorLauncher> makeNativeSuccessorLauncher();

/// How long a successor that failed to take over has to end once it is told to
/// (SuccessorLauncher::terminate()).
inline constexpr auto SuccessorTerminateWait = std::chrono::milliseconds { 2'000 };

/// How a SuccessorHandoff waits.
struct SuccessorHandoffConfig
{
    /// How long the successor has to start, read the image and say it is ready; it is ended after that, and
    /// the daemon resumes. With SuccessorTerminateWait, well within the time the clients hold on for an
    /// upgrade (client::UpgradeHold): the daemon's loop answers nothing meanwhile, and a client that stopped
    /// waiting would start a daemon of its own. The readiness probe ran the new release's executable just
    /// before, so it starts warm -- read from disk, and scanned by any virus checker already.
    std::chrono::milliseconds readyTimeout { 8'000 };
};

/// How a successor waits at the commit point (receiveHandover()).
struct SuccessorTakeoverConfig
{
    /// How long it waits, once it said it is ready, for its predecessor to commit or to end. A predecessor
    /// commits as it sees it ready: one that has not within this gave up on it -- it was not ready in time,
    /// and could not be ended -- and serves on, and the successor ends without taking anything over. Bounded,
    /// so that such a successor cannot take over later, from a predecessor that ends for another reason.
    std::chrono::milliseconds commitWait { 5'000 };
    /// How long it waits, once its predecessor committed, for it to have ended: it ends itself at once.
    std::chrono::milliseconds predecessorEndWait { 10'000 };
};

/// The Windows handoff: writes the image into an unnamed mapping, starts the target as a successor that
/// inherits it and every handle the image names, waits until it is ready, commits, and ends this process.
/// A successor that ends or is not ready in time is ended, and handOver() returns.
class SuccessorHandoff final: public ImageHandoff
{
  public:
    /// @param launcher What starts the successor, and ends a process. Not owned.
    /// @param memory Where the image is written. Not owned.
    /// @param config How long it waits.
    SuccessorHandoff(SuccessorLauncher& launcher,
                     HandoverMemory& memory,
                     SuccessorHandoffConfig config) noexcept:
        _launcher { launcher }, _memory { memory }, _config { config }
    {
    }

    [[nodiscard]] ExecFailure handOver(std::filesystem::path const& executable,
                                       std::vector<std::string> argv,
                                       HandoverImage const& image,
                                       std::span<uint8_t const> bytes) override;

  private:
    SuccessorLauncher& _launcher;
    HandoverMemory& _memory;
    SuccessorHandoffConfig _config;
};

/// The successor's side of a live upgrade on Windows, whole: reads the image the predecessor handed over in
/// its mapping, checks that every session in it can be adopted here, then holds the commit point: says it is
/// ready, and waits for the commit. Nothing is taken over before that: a successor that cannot read the
/// image, or adopt every session in it, never says it is ready, and the predecessor ends it and serves on.
/// @param handles The handles `--handover=handles=` named (parseHandoverHandles()). Closed by this.
/// @param memory What reads the mapping.
/// @param config How long it waits at the commit point.
/// @return The image to take over; or why not, in which case this process ends without serving anything.
[[nodiscard]] std::expected<HandoverImage, std::string> receiveHandover(SuccessorHandles const& handles,
                                                                        HandoverMemory& memory,
                                                                        SuccessorTakeoverConfig config = {});

#endif

} // namespace vthost::handover
