// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/CrossingHandle.hpp>
#include <vtpty/ImageSize.hpp>
#include <vtpty/PageSize.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace vtpty
{

/// A pseudoconsole (ConPTY), as it crosses to the successor process of a Windows daemon's live upgrade: the
/// handles that make it up, which the successor inherits under the same values.
///
/// Plain data, compiled on every platform so that a handover image can carry it anywhere; only
/// ConPty::adopt() turns it back into a live pseudoconsole, and that is Windows-only.
struct InheritedConPty
{
    /// This end of the pipe the pseudoconsole writes the program's output to.
    CrossingHandle outputReader {};
    /// This end of the pipe the pseudoconsole reads the program's input from.
    CrossingHandle inputWriter {};
    /// The pseudoconsole's signal pipe, which resizes and closes it.
    CrossingHandle signal {};
    /// Its reference handle, which keeps the console session open.
    CrossingHandle reference {};
    /// The console host process that serves it.
    CrossingHandle consoleHost {};

    /// @return Every handle it is made of, in the order they are declared.
    [[nodiscard]] std::array<CrossingHandle, 5> handles() const noexcept
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
    std::optional<std::pair<int, int>> stdoutFastPipe {};
    PageSize pageSize;                        ///< The terminal size at hand-off.
    std::optional<ImageSize> pixels {};       ///< The terminal size in pixels at hand-off, where known.
    std::optional<InheritedConPty> conpty {}; ///< The pseudoconsole, on Windows; nullopt elsewhere.

    /// @return Every handle or descriptor this PTY carries across on this platform: on POSIX the master, then
    ///         the fast pipe's reader and writer; on Windows the pseudoconsole's handles
    ///         (InheritedConPty::handles()). The other platform's members are never named, whatever a corrupt
    ///         handover image put in them, and each one it does not have is left out.
    [[nodiscard]] std::vector<CrossingHandle> handles() const
    {
        auto named = std::vector<CrossingHandle> {};
#ifdef _WIN32
        if (conpty)
            std::ranges::copy(conpty->handles(), std::back_inserter(named));
#else
        // A descriptor as it crosses: a negative one is none.
        auto const descriptor = [](int fd) {
            return fd < 0 ? CrossingHandle {} : CrossingHandle { fd };
        };
        named.push_back(descriptor(masterFd));
        if (stdoutFastPipe)
        {
            named.push_back(descriptor(stdoutFastPipe->first));
            named.push_back(descriptor(stdoutFastPipe->second));
        }
#endif
        std::erase_if(named, [](CrossingHandle handle) { return handle.isNone(); });
        return named;
    }
};

/// Whether this process can adopt the pseudoconsoles a daemon hands over: what a release reports of itself
/// (pseudoconsoleAdoptability()).
enum class PseudoconsoleAdoptability : std::uint8_t
{
    NotApplicable = 0, ///< No PTY of this platform is a pseudoconsole: POSIX.
    Unable, ///< The bundled conpty.dll does not load beside the executable, or lacks ConptyPackPseudoConsole.
    Able,   ///< It can.
};

/// Why a PTY or a child process could not be adopted.
enum class AdoptError : std::uint8_t
{
    BadDescriptor = 0, ///< A descriptor or handle is not open, or the master is not a terminal.
    NoSuchChild,       ///< The process id is not a child of this process (dead and reaped, or foreign).
    /// The pseudoconsole is not the bundled conpty.dll's: only that one's layout behind an HPCON is known,
    /// and only it can be rebuilt from its handles (ConPty::adopt()).
    ForeignConsoleHost,
    Unsupported, ///< A device that cannot be handed over at all (a channel, SSH, a test double).
    /// This process is out of descriptors (or the system of the objects behind them): what a PTY needs beside
    /// the inherited ones could not be made. Passes once descriptors are to be had again.
    OutOfDescriptors,
};

} // namespace vtpty
