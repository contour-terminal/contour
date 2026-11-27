// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The POSIX side of @c walkBeneath: a cursor over directory file descriptors, each opened beneath
/// the one before it without following, so a directory swapped for a link between two components
/// is met as a link, never crossed. The one exception is the shortcut, where an @c IBeneathOpener
/// is given: the components before the final name go to the kernel as one path relative to the
/// root, which `openat2(RESOLVE_BENEATH)` resolves without leaving it. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>

#include <span>
#include <string>
#include <vector>

namespace core::fs::detail
{

/// The `openat` flags a walk opens each directory with: `O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`,
/// and `O_PATH` where the system has it, so a directory the process may search but not read is
/// still walked through. Defined by the `BeneathDefault.cpp` the build compiles.
/// @return The flags.
[[nodiscard]] int walkDirectoryOpenFlags() noexcept;

/// A cursor over directory file descriptors: the root, borrowed, and a stack of the directories
/// entered beneath it, each held open by this cursor and closed where it is left.
class CORE_FS_API PosixCursor final: public IBeneathCursor
{
  public:
    /// @param root The directory the walk starts on; borrowed, and must outlive the cursor.
    /// @param opener The kernel's beneath step, offered on the root, or null.
    PosixCursor(int root, IBeneathOpener const* opener) noexcept;

    PosixCursor(PosixCursor const&) = delete;
    PosixCursor(PosixCursor&&) = delete;
    PosixCursor& operator=(PosixCursor const&) = delete;
    PosixCursor& operator=(PosixCursor&&) = delete;
    ~PosixCursor() override = default;

    [[nodiscard]] bool atRoot() const noexcept override;
    [[nodiscard]] FsResult<EntryKind> enter(std::string const& name) override;
    void leave() override;
    [[nodiscard]] FsResult<EntryKind> inspect(std::string const& name) override;
    [[nodiscard]] FsResult<std::string> readLink(std::string const& name) override;
    [[nodiscard]] FsResult<Shortcut> shortcut(std::span<std::string const> components) override;
    void restart() override;

    /// @return The directory the cursor stands on, borrowed.
    [[nodiscard]] int current() const noexcept;

    /// Hands over the directory the cursor stands on, where it is not the root.
    /// @return It; one holding no handle where the cursor stands on the root.
    [[nodiscard]] UniqueHandle release() noexcept;

  private:
    int _root;
    IBeneathOpener const* _opener;
    std::vector<UniqueHandle> _entered;
};

/// @param directory A directory.
/// @param name A name in it.
/// @return The link's target; `Invalid` where @p name is no link.
[[nodiscard]] FsResult<std::string> readLinkAt(int directory, std::string const& name);

} // namespace core::fs::detail
