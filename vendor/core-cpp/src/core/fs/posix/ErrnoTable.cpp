// SPDX-License-Identifier: Apache-2.0
//
// The errno tables of a POSIX system that is neither Apple nor a BSD (Linux, and the rest).
#include <core/fs/FsError.hpp>
#include <core/fs/posix/ErrnoRows.hpp>

#include <array>
#include <cerrno>
#include <span>

namespace core::fs
{

namespace
{

    // Linux and the others answer an O_NOFOLLOW open of a symbolic link with ELOOP alone.
    constexpr auto NoFollowSymlinkErrnos = std::to_array<int>({ ELOOP });

} // namespace

std::span<SystemErrorRow const> systemErrorTable() noexcept
{
    return detail::ErrnoRows;
}

std::span<SystemErrorRow const> ntStatusTable() noexcept
{
    return {};
}

std::span<int const> noFollowSymlinkErrnos() noexcept
{
    return NoFollowSymlinkErrnos;
}

} // namespace core::fs
