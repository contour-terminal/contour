// SPDX-License-Identifier: Apache-2.0
//
// The errno tables of Apple and the BSDs.
#include <core/fs/FsError.hpp>
#include <core/fs/posix/ErrnoRows.hpp>

#include <array>
#include <cerrno>
#include <span>

namespace core::fs
{

namespace
{

    // FreeBSD answers an O_NOFOLLOW open of a symbolic link with EMLINK, where POSIX says ELOOP and
    // macOS and the other BSDs answer that. EMLINK cannot come from an open otherwise, so the row costs
    // the systems that never send it nothing.
    constexpr auto NoFollowSymlinkErrnos = std::to_array<int>({ ELOOP, EMLINK });

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
