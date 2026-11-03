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

/// A PTY master, as it crosses an exec of the daemon (live upgrade) in the descriptors the new image
/// inherits.
///
/// Plain data, compiled on every platform so that a handover image can carry it anywhere; only
/// UnixPty::adopt() turns it back into a live PTY, and that is POSIX-only.
struct InheritedPty
{
    int masterFd = -1; ///< The PTY master descriptor.
    /// The reader and writer ends of the stdout fast pipe, where the PTY has one. Either end is -1 where
    /// this process has closed it already (the parent closes the writer after spawning).
    std::optional<std::pair<int, int>> stdoutFastPipe;
    PageSize pageSize;               ///< The terminal size at hand-off.
    std::optional<ImageSize> pixels; ///< The terminal size in pixels at hand-off, where known.

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
    BadDescriptor = 0, ///< A descriptor is not open, or the master is not a terminal.
    NoSuchChild,       ///< The process id is not a child of this process (dead and reaped, or foreign).
};

} // namespace vtpty
