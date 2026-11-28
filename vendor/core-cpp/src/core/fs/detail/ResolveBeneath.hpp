// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a native descriptor resolves a path beneath itself: the @c walkBeneath walk over directory
/// handles it holds, with the one step a kernel may take for it (`openat2(RESOLVE_BENEATH)` on
/// Linux) behind @c IBeneathOpener. Which opener `BeneathStrategy::Best` stands for is decided by
/// the one `BeneathDefault.cpp` the build compiles. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/platform/Types.hpp>

#include <memory>
#include <string>

namespace core::fs::detail
{

/// Opens a directory several components beneath a held one in a single call, refusing whatever
/// would leave it, where the kernel can be asked to.
class IBeneathOpener
{
  public:
    IBeneathOpener() = default;
    IBeneathOpener(IBeneathOpener const&) = delete;
    IBeneathOpener(IBeneathOpener&&) = delete;
    IBeneathOpener& operator=(IBeneathOpener const&) = delete;
    IBeneathOpener& operator=(IBeneathOpener&&) = delete;
    virtual ~IBeneathOpener() = default;

    /// @param root The directory @p path is relative to.
    /// @param path Components joined by `/`: names and `..`, no final name to act on.
    /// @return The directory, held; one holding no handle where the call declines and the caller
    ///         should walk instead; `NotPermitted` where @p path leaves @p root, and what else
    ///         refused it.
    [[nodiscard]] virtual FsResult<UniqueHandle> openDirectoryBeneath(platform::NativeHandle root,
                                                                      std::string const& path) const = 0;
};

/// What `BeneathStrategy::Best` stands for on this system, decided once, when a file system is
/// built: the `openat2` opener where Linux has the call, and none (the walk alone) elsewhere.
/// Defined by the `BeneathDefault.cpp` the build compiles.
/// @return The opener, or null.
[[nodiscard]] std::unique_ptr<IBeneathOpener const> makeBestBeneathOpener();

/// Where a path resolved to.
struct Resolved
{
    UniqueHandle held;                                     ///< The directory, where the walk left the root.
    platform::NativeHandle root = platform::InvalidHandle; ///< The root it started from, borrowed.
    std::string name;                                    ///< The final name; empty for the directory itself.
    EntryKind kind = EntryKind::Missing;                 ///< What the name is, unfollowed.
    FinalComponent finalComponent = FinalComponent::Any; ///< What the path asked it to be.

    /// @return The directory that holds @c name: @c held, or the root.
    [[nodiscard]] platform::NativeHandle directory() const noexcept
    {
        return held.get() != platform::InvalidHandle ? held.get() : root;
    }
};

/// Resolves @p plan beneath @p root. The final name is never resolved: it is acted on, by the
/// caller, without following, in @c Resolved::directory.
/// @param root The descriptor's directory handle; borrowed for the call.
/// @param plan The planned path.
/// @param rules How links, `..` and the kernel's separators are treated.
/// @param opener The kernel's beneath step, or null to walk one component at a time.
/// @return Where it resolved to, or what @c walkBeneath answers.
[[nodiscard]] CORE_FS_API FsResult<Resolved> resolveBeneath(platform::NativeHandle root,
                                                            PathPlan plan,
                                                            WalkRules const& rules,
                                                            IBeneathOpener const* opener);

} // namespace core::fs::detail
