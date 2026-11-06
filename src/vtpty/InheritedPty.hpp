// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ImageSize.hpp>
#include <vtpty/PageSize.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace vtpty
{

/// A handle's value, as it crosses to another process: a Windows HANDLE widened to an integer, 0 for none.
/// Windows keeps every kernel handle within 32 bits, so the value survives any round trip through it.
using HandleValue = std::uint64_t;

/// A pseudoconsole (ConPTY), as it crosses to the successor process of a Windows daemon's live upgrade: the
/// handles that make it up, which the successor inherits under the same values.
///
/// Plain data, compiled on every platform so that a handover image can carry it anywhere; only
/// ConPty::adopt() turns it back into a live pseudoconsole, and that is Windows-only.
struct InheritedConPty
{
    HandleValue outputReader = 0; ///< This end of the pipe the pseudoconsole writes the program's output to.
    HandleValue inputWriter = 0;  ///< This end of the pipe the pseudoconsole reads the program's input from.
    HandleValue signal = 0;       ///< The pseudoconsole's signal pipe, which resizes and closes it.
    HandleValue reference = 0;    ///< Its reference handle, which keeps the console session open.
    HandleValue consoleHost = 0;  ///< The console host process that serves it.

    /// @return Every handle it is made of, in the order they are declared.
    [[nodiscard]] std::array<HandleValue, 5> handles() const noexcept
    {
        return { outputReader, inputWriter, signal, reference, consoleHost };
    }
};

/// A PTY, as it crosses to the daemon that takes over in a live upgrade: on POSIX the master in the
/// descriptors the new image inherits across the exec, on Windows the pseudoconsole's handles in the ones
/// the successor process inherits.
///
/// Plain data, compiled on every platform so that a handover image can carry it anywhere; only
/// UnixPty::adopt() and ConPty::adopt() turn it back into a live PTY, each on its own platform.
struct InheritedPty
{
    int masterFd = -1; ///< The PTY master descriptor; -1 on Windows.
    /// The reader and writer ends of the stdout fast pipe, where the PTY has one. Either end is -1 where
    /// this process has closed it already (the parent closes the writer after spawning).
    std::optional<std::pair<int, int>> stdoutFastPipe;
    PageSize pageSize;                        ///< The terminal size at hand-off.
    std::optional<ImageSize> pixels;          ///< The terminal size in pixels at hand-off, where known.
    std::optional<InheritedConPty> conpty {}; ///< The pseudoconsole, on Windows; nullopt elsewhere.

    /// @return Every descriptor this PTY carries across the exec: the master, then the fast pipe's reader and
    ///         writer; -1 for each one it does not have.
    [[nodiscard]] std::array<int, 3> descriptors() const noexcept
    {
        if (!stdoutFastPipe)
            return { masterFd, -1, -1 };
        return { masterFd, stdoutFastPipe->first, stdoutFastPipe->second };
    }
};

/// Why a PTY or a child process could not be adopted.
enum class AdoptError : std::uint8_t
{
    BadDescriptor = 0, ///< A descriptor or handle is not open, or the master is not a terminal.
    NoSuchChild,       ///< The process id is not a child of this process (dead and reaped, or foreign).
    /// The pseudoconsole is not the bundled conpty.dll's: only that one's layout behind an HPCON is known,
    /// and only it can be rebuilt from its handles (ConPty::adopt()).
    ForeignConsoleHost,
};

} // namespace vtpty
