// SPDX-License-Identifier: Apache-2.0
//
// The inheritable ACL of Linux and every POSIX system without one of its own: a POSIX.1e default ACL (setfacl
// -d) granting other, and the user nobody, everything.
#include <core/fs/posix/InheritableAcl.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace core::fs::test
{

std::vector<std::string> inheritableAclCommand(std::filesystem::path const& directory)
{
    return { "setfacl", "-d", "-m", "u::rwx,g::rwx,o::rwx,u:nobody:rwx,m::rwx", directory.string() };
}

} // namespace core::fs::test
