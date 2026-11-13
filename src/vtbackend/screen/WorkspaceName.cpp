// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/WorkspaceName.hpp>

#include <core/Utf8Path.hpp>

#include <string>
#include <utility>

namespace vtbackend
{

namespace
{
    /// @return @p path's last component, unless it is a separator or a dot; else empty.
    std::string nameOf(std::filesystem::path const& path)
    {
        auto const name = path.filename();
        if (name.empty() || name == "." || name == "..")
            return {};
        return core::pathToUtf8(name);
    }
} // namespace

std::string defaultWorkspaceName(GitFileSystem const& fileSystem, std::filesystem::path const& cwd)
{
    if (cwd.empty())
        return {};
    auto normal = cwd.lexically_normal();
    if (!normal.has_filename())
        normal = normal.parent_path(); // "/a/b/" names "/a/b"
    // The repository is found as git's own discovery finds it; the folder holding its `.git` is then the
    // first one up the path that has one, which a linked worktree's own top folder is, too.
    if (findGitHeadFile(fileSystem, normal))
    {
        for (auto current = normal; current.has_relative_path(); current = current.parent_path())
            if (fileSystem.status(current / ".git").kind != GitPathKind::Absent)
                return nameOf(current);
    }
    return nameOf(normal);
}

} // namespace vtbackend
