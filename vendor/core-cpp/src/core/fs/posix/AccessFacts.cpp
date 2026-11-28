// SPDX-License-Identifier: Apache-2.0
//
// Who may reach an object on a BSD with no ACL beyond the mode (OpenBSD, DragonFly): the owner and
// the mode of an fstat. Nothing else is read, so the facts hold for an fstatat by name as for a
// handle.
#include <core/fs/AccessScope.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <optional>

namespace core::fs::detail
{

std::optional<AccessFacts> accessFactsOf(int /*handle*/, struct ::stat const& status)
{
    return modeFactsOf(status);
}

} // namespace core::fs::detail
