// SPDX-License-Identifier: Apache-2.0
//
// Who may reach an object where this module reads no ACL the system may keep: NetBSD, whose FFS keeps
// NFSv4 ACLs the mode does not bound, and any POSIX system CMake names no other file for. The mode
// alone could call an object owner-only that an ACL opens to anyone, so nothing is judged: every
// answer is nullopt, and an owner-only creation, which reads its access back, is refused.
#include <core/fs/AccessScope.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <optional>

namespace core::fs::detail
{

std::optional<AccessFacts> accessFactsOf(int /*handle*/, struct ::stat const& /*status*/)
{
    return std::nullopt;
}

} // namespace core::fs::detail
