// SPDX-License-Identifier: Apache-2.0
//
// FreeBSD's inheritable ACL: an NFSv4 entry (setfacl -m on ZFS, or UFS mounted nfsv4acls) granting everyone@
// every right, inherited by files and directories.
#include <core/fs/posix/InheritableAcl.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace core::fs::test
{

std::vector<std::string> inheritableAclCommand(std::filesystem::path const& directory)
{
    return { "setfacl", "-m", "everyone@:full_set:fd:allow", directory.string() };
}

} // namespace core::fs::test
