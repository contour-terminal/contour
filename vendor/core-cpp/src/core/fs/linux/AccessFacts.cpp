// SPDX-License-Identifier: Apache-2.0
//
// Who may reach an object on Linux: the owner and the mode of an fstat, read through the very handle.
// A POSIX.1e ACL needs no reading of its own: its named entries are bounded by the mask, which the
// mode's group bits show. An NFSv4 ACL is bounded by nothing in the mode, so where one may decide --
// an NFS or SMB mount, whose server's ACL the mode only approximates, or an object that carries one
// (ZFS with acltype=nfsv4, an NFSv4 export) -- nothing is judged. Both are told from the handle, so a
// stat by name, which holds none, is not judged either.
#include <core/fs/AccessScope.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <optional>

namespace core::fs::detail
{

namespace
{

    /// The file systems whose access a server decides by an ACL the mode does not bound: NFS (every
    /// version shares the magic), and SMB in each of its three magics.
    constexpr auto AclFileSystemMagics = std::to_array<std::uint64_t>({
        0x6969,     // NFS_SUPER_MAGIC
        0xFF534D42, // CIFS_SUPER_MAGIC
        0xFE534D42, // SMB2_SUPER_MAGIC
        0x517B,     // SMB_SUPER_MAGIC
    });

    /// The extended attributes an object with an NFSv4 ACL answers: the NFS client's, and OpenZFS's.
    constexpr auto Nfs4AclAttributes = std::to_array<char const*>({
        "system.nfs4_acl",
        "system.nfs4_acl_xdr",
    });

    /// @return Whether @p handle's object carries @p attribute: a size, or a buffer too small for it.
    [[nodiscard]] bool carries(int handle, char const* attribute)
    {
        return ::fgetxattr(handle, attribute, nullptr, 0) >= 0 || errno == ERANGE;
    }

} // namespace

std::optional<AccessFacts> accessFactsOf(int handle, struct ::stat const& status)
{
    // The file system is read from a handle or not at all: by name, another could be mounted there.
    if (handle < 0)
        return std::nullopt;
    struct ::statfs fileSystem {};
    if (::fstatfs(handle, &fileSystem) != 0)
        return std::nullopt;
    if (std::ranges::find(AclFileSystemMagics, static_cast<std::uint64_t>(fileSystem.f_type))
        != AclFileSystemMagics.end())
        return std::nullopt;
    if (std::ranges::any_of(Nfs4AclAttributes,
                            [&](char const* attribute) { return carries(handle, attribute); }))
        return std::nullopt;
    return modeFactsOf(status);
}

} // namespace core::fs::detail
