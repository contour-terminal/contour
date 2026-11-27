// SPDX-License-Identifier: Apache-2.0

/// @file
/// `classifyStreamHandle` on POSIX: `fstat`'s file type, looked up in one table.

#include <core/net/StandardStreams.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include <unistd.h>

namespace core::net
{

namespace
{
    /// What else a file type needs before it is Pollable.
    enum class Admission : std::uint8_t
    {
        Always,       ///< Its type is enough.
        TerminalOnly, ///< Only a terminal: a character device that is not one (`/dev/null`) is not.
    };

    /// One file type, and how a stream over it is driven.
    struct StreamKindRow
    {
        mode_t type;         ///< `S_IFMT`'s value for it.
        StreamKind kind;     ///< How it is driven when admitted.
        Admission admission; ///< What else it needs.
    };

    /// The file types a standard handle can be. A type not listed -- a regular file, a block device,
    /// a directory -- is Blocking: a regular file is always "readable", so the loop can do nothing
    /// for it, and epoll refuses one outright with EPERM.
    ///
    /// A character device is Pollable only as a terminal. `/dev/null` is one, and epoll refuses it
    /// too; `/dev/zero` and `/dev/urandom` are always readable, which waiting tells nothing about.
    constexpr auto StreamKindTable = std::array {
        StreamKindRow { .type = S_IFIFO, .kind = StreamKind::Pollable, .admission = Admission::Always },
        StreamKindRow { .type = S_IFSOCK, .kind = StreamKind::Pollable, .admission = Admission::Always },
        StreamKindRow { .type = S_IFCHR, .kind = StreamKind::Pollable, .admission = Admission::TerminalOnly },
    };
} // namespace

StreamKind classifyStreamHandle(platform::NativeHandle handle) noexcept
{
    struct stat status {};
    if (::fstat(handle, &status) != 0)
        return StreamKind::Blocking;
    auto const type = static_cast<mode_t>(status.st_mode & S_IFMT);
    auto const row = std::ranges::find(StreamKindTable, type, &StreamKindRow::type);
    if (row == StreamKindTable.end())
        return StreamKind::Blocking;
    if (row->admission == Admission::TerminalOnly && ::isatty(handle) != 1)
        return StreamKind::Blocking;
    return row->kind;
}

} // namespace core::net
