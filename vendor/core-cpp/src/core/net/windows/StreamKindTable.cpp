// SPDX-License-Identifier: Apache-2.0

/// @file
/// `classifyStreamHandle` on Windows: `GetFileType`, looked up in one table.

#include <core/net/StandardStreams.hpp>

#include <algorithm>
#include <array>

#include <windows.h>

namespace core::net
{

namespace
{
    /// One file type, and how a stream over it is driven.
    struct StreamKindRow
    {
        DWORD type;      ///< What `GetFileType` answers.
        StreamKind kind; ///< How it is driven.
    };

    /// Every row is Blocking, each for its own reason, and written down so the reason is not lost
    /// the day one of them could be driven by the loop. A type not listed (`FILE_TYPE_UNKNOWN`, an
    /// absent or invalid handle) is Blocking too.
    constexpr auto StreamKindTable = std::array {
        // A console (or `NUL`): a console handle cannot be associated with a completion port, and
        // waiting on its signalled state wakes for mouse and focus events a read would block on.
        StreamKindRow { .type = FILE_TYPE_CHAR, .kind = StreamKind::Blocking },
        // A file: always readable, so there is nothing to wait for.
        StreamKindRow { .type = FILE_TYPE_DISK, .kind = StreamKind::Blocking },
        // A pipe (a socket too): an inherited anonymous pipe is synchronous, and a handle opened
        // without FILE_FLAG_OVERLAPPED cannot be reopened as overlapped by its child.
        StreamKindRow { .type = FILE_TYPE_PIPE, .kind = StreamKind::Blocking },
    };
} // namespace

StreamKind classifyStreamHandle(platform::NativeHandle handle) noexcept
{
    auto const type = GetFileType(handle);
    auto const row = std::ranges::find(StreamKindTable, type, &StreamKindRow::type);
    if (row == StreamKindTable.end())
        return StreamKind::Blocking;
    return row->kind;
}

} // namespace core::net
