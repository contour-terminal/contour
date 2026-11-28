// SPDX-License-Identifier: Apache-2.0
//
// macOS's inheritable ACL: an extended ACL entry (chmod +a) granting everyone read and write, inherited by
// the files and directories created inside.
#include <core/fs/posix/InheritableAcl.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace core::fs::test
{

std::vector<std::string> inheritableAclCommand(std::filesystem::path const& directory)
{
    return {
        "chmod", "+a", "everyone allow read,write,append,file_inherit,directory_inherit", directory.string()
    };
}

} // namespace core::fs::test
