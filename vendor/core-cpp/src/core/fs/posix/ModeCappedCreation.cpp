// SPDX-License-Identifier: Apache-2.0
//
// Owner-only creation where the creating call's mode caps whatever a directory hands down: Linux,
// whose POSIX.1e default ACL is ANDed with the mode, and every POSIX system whose ACLs are not read
// here. Each fills its AccessFacts in its own AccessFacts.cpp.
#include <core/fs/posix/OwnerOnlyCreation.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

namespace core::fs::detail
{

FsResult<void> stripInheritedAcl(int /*handle*/, ::mode_t /*mode*/)
{
    // A POSIX.1e default ACL replaces the umask step, but each entry it hands down is ANDed with the
    // mode argument's bits: the mask with the group bits, other with the other bits. Under 0600 or
    // 0700 both are zero, so no named entry and no other is left any right; nothing to strip.
    return {};
}

OwnerOnlyCreation nativeOwnerOnlyCreation() noexcept
{
    // The mode caps every entry a default ACL hands down.
    return OwnerOnlyCreation::Direct;
}

} // namespace core::fs::detail
