// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Asks on the terminal whether to pin a daemon met for the first time, as ssh asks about a new host's key.

#include <core/async/Task.hpp>
#include <core/platform/Types.hpp>

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

#include <vthost/KnownDaemons.hpp>

namespace contour::cli
{

/// Whether there is a person at the terminal to ask.
enum class TerminalAttendance : std::uint8_t
{
    Unattended = 0, ///< Standard input or standard error is not a terminal: nobody can be asked.
    Attended,       ///< Both are a terminal.
};

/// @return Whether this process's standard input and standard error are both a terminal (on Windows: both a
/// console).
[[nodiscard]] TerminalAttendance currentTerminalAttendance() noexcept;

/// Asks `Trust this daemon? [y/N]` after showing the daemon's address and fingerprint; anything but `y` or
/// `yes` (any case), and the end of the input, declines. The answer is read on a thread of its own, so that
/// the loop's timers and its stop go on while the user types. The thread belongs to the wait: a wait that is
/// cancelled wakes it and joins it, so no read outlives the prompt, and the process can end at once. On
/// Windows it waits on the console's input handle and the wakeup together; a line the user has begun is read
/// to its Enter, which a cancelled wait types for them, so the thread is joined there too.
class TerminalPinPrompt final: public vthost::IPinPrompt
{
  public:
    /// @param input The descriptor the answer is read from (standard input); borrowed, and must outlive this
    ///        object.
    /// @param out Where the question is written (standard error); borrowed, and must outlive this object.
    TerminalPinPrompt(core::platform::NativeHandle input, std::ostream& out);

    /// @copydoc vthost::IPinPrompt::confirm
    [[nodiscard]] core::async::Task<vthost::PinDecision> confirm(std::string hostPort,
                                                                 std::string fingerprint) override;

  private:
    core::platform::NativeHandle _input;
    std::ostream& _out;
};

/// @param attendance Whether anyone is at the terminal.
/// @param input The descriptor the answer is read from; borrowed.
/// @param out Where the question is written; borrowed.
/// @return A TerminalPinPrompt over them, or null for Unattended, which leaves a first contact to
///         `--daemon-fingerprint`.
[[nodiscard]] std::shared_ptr<vthost::IPinPrompt> makeTerminalPinPrompt(TerminalAttendance attendance,
                                                                        core::platform::NativeHandle input,
                                                                        std::ostream& out);

} // namespace contour::cli
