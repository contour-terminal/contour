// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/CrossingHandle.hpp>

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vtpty
{

/// How long Process::close() gives a child that the closing terminal hung up to end, before it kills
/// the child; on POSIX only, as on Windows closing the pseudoconsole ends the child outright.
///
/// Nothing tells the grace that the child has ended -- nobody need be waiting for it -- so it asks:
/// whether the child has ended, by waitpid(), and whether the grace has passed, by @ref clock, about
/// every 10 ms. It ends at whichever comes first.
struct HangupGrace
{
    /// wezterm's grace for the same hangup.
    static constexpr auto Default = std::chrono::milliseconds { 200 };

    std::chrono::milliseconds duration = Default; ///< How long the child is given.

    /// The clock the grace is measured on. It must outlive the Process.
    std::reference_wrapper<core::platform::IClock const> clock = core::platform::defaultSteadyClock();
};

/// A running child process a previous image of this process left behind across an exec (live upgrade), or
/// the daemon a Windows successor takes over from left it.
///
/// Plain data, compiled on every platform so a handover image can carry it anywhere; only
/// Process::adopt() turns it back into a Process. On POSIX the child stays a child of this process across
/// exec (the process id is kept), so waiting for it works as for a spawned one; on Windows the successor
/// inherits a handle to it (processHandle), which is what it waits on.
struct AdoptedChild
{
    std::int64_t pid = -1;            ///< The child's process id.
    std::string executable {};        ///< The program it runs, as spawned.
    std::vector<std::string> args {}; ///< Its arguments, as spawned.
    std::filesystem::path cwd {};     ///< Its working directory, as spawned.
    HangupGrace hangupGrace {};       ///< The grace close() gives it.
    CrossingHandle processHandle {};  ///< On Windows, the handle to the process that crosses; none elsewhere.
};

} // namespace vtpty
