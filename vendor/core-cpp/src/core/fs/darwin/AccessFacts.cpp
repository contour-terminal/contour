// SPDX-License-Identifier: Apache-2.0
//
// Who may reach an object on macOS: the owner and the mode of an fstat, and the extended ACL of the
// very handle. macOS keeps no ACL equivalent to the mode, so any entry at all is judged Shared (an
// ALLOW for another user would be the one that matters, but a reader that guesses fails open).
#include <core/fs/AccessScope.hpp>
#include <core/fs/posix/OwnerOnlyCreation.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
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

    /// What a file system without extended ACLs answers an ACL call with.
    constexpr auto NoAclErrnos = std::to_array({ ENOTSUP, EOPNOTSUPP });

    [[nodiscard]] bool isNoAcl(int error) noexcept
    {
        return std::ranges::find(NoAclErrnos, error) != NoAclErrnos.end();
    }

    /// Whether @p handle's file system keeps extended ACLs at all (`_PC_EXTENDED_SECURITY_NP`, as
    /// FreeBSD's reader asks `_PC_ACL_NFS4`); nullopt where it cannot be asked.
    [[nodiscard]] std::optional<bool> keepsAcls(int handle) noexcept
    {
        // errno is cleared first: -1 with errno untouched answers nothing, and fails closed.
        errno = 0;
        auto const answer = ::fpathconf(handle, _PC_EXTENDED_SECURITY_NP);
        if (answer > 0)
            return true;
        if (answer == 0 || errno == EINVAL)
            return false;
        return std::nullopt;
    }

} // namespace

std::optional<AccessFacts> accessFactsOf(int handle, struct ::stat const& status)
{
    // An ACL is read from a handle or not at all: by name, another user could rename in between.
    if (handle < 0)
        return std::nullopt;
    auto facts = modeFactsOf(status);
    // A file system without extended ACLs (some FAT, SMB, NFS mounts) leaves the mode all there is.
    auto const keeps = keepsAcls(handle);
    if (!keeps.has_value())
        return std::nullopt;
    if (!*keeps)
        return facts;
    auto const acl = std::unique_ptr<std::remove_pointer_t<acl_t>, AclDeleter> { ::acl_get_fd_np(
        handle, ACL_TYPE_EXTENDED) };
    if (acl == nullptr)
    {
        // ENOENT is the answer for an object with no extended ACL, ENOTSUP for a file system with
        // none; any other failure tells nothing.
        if (errno == ENOENT || isNoAcl(errno))
            return facts;
        return std::nullopt;
    }
    auto entry = acl_entry_t {};
    if (::acl_get_entry(acl.get(), ACL_FIRST_ENTRY, &entry) != 0)
        return facts;
    facts.acl = AccessFacts::AclState::Present;
    facts.entries.push_back(AccessEntry {});
    return facts;
}

FsResult<void> stripInheritedAcl(int handle, ::mode_t /*mode*/)
{
    // A file system without extended ACLs handed nothing down: the mode alone is verified after.
    auto const keeps = keepsAcls(handle);
    if (!keeps.has_value())
        return errnoError("fpathconf(_PC_EXTENDED_SECURITY_NP)");
    if (!*keeps)
        return {};
    // An empty extended ACL replaces whatever the new object inherited (acl_set_fd_np(3)); the
    // mode was never touched by the inheritance.
    auto const empty = std::unique_ptr<std::remove_pointer_t<acl_t>, AclDeleter> { ::acl_init(0) };
    if (empty == nullptr)
        return errnoError("acl_init");
    if (::acl_set_fd_np(handle, empty.get(), ACL_TYPE_EXTENDED) != 0 && !isNoAcl(errno))
        return errnoError("acl_set_fd_np(ACL_TYPE_EXTENDED)");
    return {};
}

OwnerOnlyCreation nativeOwnerOnlyCreation() noexcept
{
    // An extended ACL a directory hands down can widen a new object.
    return OwnerOnlyCreation::Staged;
}

} // namespace core::fs::detail
