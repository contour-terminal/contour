// SPDX-License-Identifier: Apache-2.0
//
// Who may reach an object on FreeBSD: the owner and the mode of an fstat, and where the file system
// keeps NFSv4 ACLs (ZFS, UFS mounted with nfsv4acls), the ACL of the very handle. A trivial one says
// no more than the mode; any other is judged Shared, since its entries can grant anyone. A POSIX.1e
// ACL, UFS's other kind, is bounded by the mode's group bits, as on Linux.
#include <core/fs/AccessScope.hpp>
#include <core/fs/posix/OwnerOnlyCreation.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <memory>
#include <optional>
#include <type_traits>

namespace core::fs::detail
{

namespace
{

    struct AclDeleter
    {
        void operator()(std::remove_pointer_t<acl_t>* acl) const noexcept { ::acl_free(acl); }
    };

} // namespace

std::optional<AccessFacts> accessFactsOf(int handle, struct ::stat const& status)
{
    // An ACL is read from a handle or not at all: by name, another user could rename in between.
    if (handle < 0)
        return std::nullopt;
    auto facts = modeFactsOf(status);
    errno = 0; // -1 with errno untouched answers nothing, and fails closed below.
    auto const nfs4 = ::fpathconf(handle, _PC_ACL_NFS4);
    // 0, or EINVAL where the file system does not know the variable at all, is no NFSv4 ACL.
    if (nfs4 == 0 || (nfs4 < 0 && errno == EINVAL))
        return facts;
    if (nfs4 < 0)
        return std::nullopt;
    auto const acl =
        std::unique_ptr<std::remove_pointer_t<acl_t>, AclDeleter> { ::acl_get_fd_np(handle, ACL_TYPE_NFS4) };
    if (acl == nullptr)
        return std::nullopt;
    auto trivial = 0;
    if (::acl_is_trivial_np(acl.get(), &trivial) != 0)
        return std::nullopt;
    if (trivial != 0)
    {
        facts.acl = AccessFacts::AclState::Trivial;
        return facts;
    }
    // Its entries are not judged one by one: any non-trivial ACL can grant someone else.
    facts.acl = AccessFacts::AclState::Present;
    facts.entries.push_back(AccessEntry {});
    return facts;
}

FsResult<void> stripInheritedAcl(int handle, ::mode_t mode)
{
    // Only an NFSv4 ACL inherits entries beyond the mode (ZFS aclinherit, UFS nfsv4acls); a
    // POSIX.1e default ACL is capped by the mode, as on Linux.
    errno = 0; // -1 with errno untouched answers nothing, and fails closed below.
    auto const nfs4 = ::fpathconf(handle, _PC_ACL_NFS4);
    if (nfs4 == 0 || (nfs4 < 0 && errno == EINVAL))
        return {};
    if (nfs4 < 0)
        return errnoError("fpathconf(_PC_ACL_NFS4)");
    auto const inherited =
        std::unique_ptr<std::remove_pointer_t<acl_t>, AclDeleter> { ::acl_get_fd_np(handle, ACL_TYPE_NFS4) };
    if (inherited == nullptr)
        return errnoError("acl_get_fd_np(ACL_TYPE_NFS4)");
    // acl_strip_np(3): the trivial ACL, equivalent to the mode alone. Then the mode again, which ZFS
    // may have recomputed from the inherited entries.
    auto const trivial =
        std::unique_ptr<std::remove_pointer_t<acl_t>, AclDeleter> { ::acl_strip_np(inherited.get(), 0) };
    if (trivial == nullptr)
        return errnoError("acl_strip_np");
    if (::acl_set_fd_np(handle, trivial.get(), ACL_TYPE_NFS4) != 0)
        return errnoError("acl_set_fd_np(ACL_TYPE_NFS4)");
    if (::fchmod(handle, mode) != 0)
        return errnoError("fchmod");
    return {};
}

OwnerOnlyCreation nativeOwnerOnlyCreation() noexcept
{
    // An NFSv4 ACL a directory hands down can widen a new object.
    return OwnerOnlyCreation::Staged;
}

} // namespace core::fs::detail
