// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a relative path becomes before any directory is touched: its components, checked against a
/// @c PathPolicy, and the splice that replaces a symbolic link's component with its target; and the
/// one walk every resolver runs over them, one component at a time over held directories, so the
/// kernel never resolves more than one name. The walk owns the rules a resolver must not get
/// wrong on its own: `..` never climbs above the root, a final link is followed only where the call
/// or a trailing `/` says so, and a resolution follows at most @c WalkRules::maxSymlinkHops links.

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/PathPolicy.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::fs::detail
{

/// What a path's last component must be.
enum class FinalComponent : std::uint8_t
{
    Any,       ///< Whatever it names.
    Directory, ///< A directory, followed through a symbolic link: the path ends in `/` or `/.`.
};

/// A relative path split into the components a walker resolves in order.
struct PathPlan
{
    /// The components: never empty, and never `.` but as the last one, where the path ends in `.`
    /// (`.`, `a/.`): that names the directory itself rather than an entry of the one above it, so
    /// removing it is `Invalid` and creating it `Exist`. `..` is kept, for the walker to resolve
    /// against the directories it holds.
    std::vector<std::string> components;
    /// What the last component must be. A walker follows a final symbolic link where it says
    /// `Directory`, whatever the call's @c SymlinkPolicy, and answers `NotDirectory` for anything
    /// else there; an operation that would create a non-directory there answers `NotDirectory`.
    FinalComponent finalComponent = FinalComponent::Any;
};

/// @param text Bytes that should spell UTF-8.
/// @return Whether they do: no overlong form, no surrogate, nothing above U+10FFFF.
[[nodiscard]] bool isUtf8(std::string_view text) noexcept;

/// Splits @p path into components under @p policy's rules.
/// @param path A relative path in UTF-8.
/// @param policy The name rules.
/// @return The plan; `NoEntry` for an empty path, `Invalid` for a NUL, a forbidden character or a
///         refused trailing dot or space, `IllegalByteSequence` where @p path is not UTF-8, and
///         `NotPermitted` for an absolute path or a reserved name.
[[nodiscard]] CORE_FS_API FsResult<PathPlan> planRelativePath(std::string_view path,
                                                              PathPolicy const& policy);

/// Checks a symbolic link's target before the link is created, as @c spliceSymlink would on
/// following it.
/// @param target The target, split on @p policy's target separators.
/// @param policy The name rules.
/// @return Nothing, or `NoEntry` for an empty target, `NotPermitted` for an absolute one (a leading
///         separator, or a first component naming a volume), and what @c planRelativePath answers for
///         a bad component.
[[nodiscard]] CORE_FS_API FsResult<void> checkSymlinkTarget(std::string_view target,
                                                            PathPolicy const& policy);

/// What `readlinkAt` hands out: a target read from a link, unless it names a root, which no walk
/// follows and no `symlinkAt` makes (@c isRootedLinkTarget), and is refused as WASI 0.3.2 refuses it.
/// @param target The target as stored.
/// @param policy The name rules.
/// @return @p target, or `NotPermitted` where it is rooted.
[[nodiscard]] CORE_FS_API FsResult<std::string> refuseRootedTarget(std::string target,
                                                                   PathPolicy const& policy);

/// Replaces component @p at of @p plan, a symbolic link, with the components of its @p target, so
/// the walk goes on from the directory holding the link. A target ending in `/` or `/.` that
/// replaces the last component makes the plan's @c PathPlan::finalComponent `Directory`.
/// @param plan The plan being walked; left as it was where this fails.
/// @param at The index of the link's component.
/// @param target The link's target, split on @p policy's target separators.
/// @param policy The name rules.
/// @return Nothing, or why the target cannot be followed: `NoEntry` for an empty one, `NotPermitted`
///         for an absolute one, and what @c planRelativePath answers for a bad component; `Invalid`
///         where @p at is not a component of @p plan.
[[nodiscard]] CORE_FS_API FsResult<void> spliceSymlink(PathPlan& plan,
                                                       std::size_t at,
                                                       std::string_view target,
                                                       PathPolicy const& policy);

/// What a name in a directory is, as a walk sees it without following it.
enum class EntryKind : std::uint8_t
{
    Missing,      ///< Nothing has the name.
    Directory,    ///< A directory.
    SymbolicLink, ///< A symbolic link.
    Other,        ///< A regular file, a device, a FIFO or a socket.
};

/// What a trailing `/` does to a final symbolic link under `NoFollow`. A lookup follows it, as
/// POSIX resolves `dirlink/` to the directory; an operation that removes or moves the final entry
/// acts on the link itself, so Linux refuses `rmdir`, `rename` and `unlink` of `dirlink/` with
/// `ENOTDIR` rather than reaching through to the directory.
enum class TrailingSlashOnLink : std::uint8_t
{
    Follow, ///< Follow it (stat, open, set times, hash, and every call that creates an entry).
    Refuse, ///< `NotDirectory` (remove, rename, unlink).
};

/// How one resolution treats what it meets.
struct WalkRules
{
    PathPolicy const* policy = nullptr;                       ///< How a link's target is split; never null.
    SymlinkPolicy finalPolicy = SymlinkPolicy::NoFollow;      ///< Whether a final link is followed.
    TrailingSlashOnLink onLink = TrailingSlashOnLink::Follow; ///< A trailing `/` on a final link.
    std::size_t maxSymlinkHops = DefaultMaxSymlinkHops;       ///< Links followed before `Loop`.
    /// What the kernel under the cursor would split a name on: a component holding one is
    /// `Invalid`, so no policy can hand the kernel two names as one. Empty where nothing splits.
    std::string_view kernelSeparators;
};

/// Whether a cursor took a shortcut it was offered.
enum class Shortcut : std::uint8_t
{
    Declined, ///< It did not; the walk goes on one component at a time.
    Taken,    ///< It stands on the directory the components name.
};

/// The directories a walk moves through, each held open (a handle, an inode), never named again by
/// a path, with one exception: a @c shortcut names the components before the final one as a path,
/// relative to the root, to a kernel that resolves it beneath the root itself and refuses whatever
/// would leave it (`openat2(RESOLVE_BENEATH)`). It starts on the root and keeps a stack of what it
/// entered.
class IBeneathCursor
{
  public:
    IBeneathCursor() = default;
    IBeneathCursor(IBeneathCursor const&) = delete;
    IBeneathCursor(IBeneathCursor&&) = delete;
    IBeneathCursor& operator=(IBeneathCursor const&) = delete;
    IBeneathCursor& operator=(IBeneathCursor&&) = delete;
    virtual ~IBeneathCursor() = default;

    /// @return Whether the cursor stands on the root, where `..` is refused.
    [[nodiscard]] virtual bool atRoot() const noexcept = 0;

    /// Enters @p name of the current directory without following it.
    /// @param name One component, neither `.` nor `..`.
    /// @return `Directory` where it entered it, `SymbolicLink` where @p name is a link (not
    ///         entered); `NoEntry`, `NotDirectory` or what else refused it otherwise.
    [[nodiscard]] virtual FsResult<EntryKind> enter(std::string const& name) = 0;

    /// Goes back to the directory the current one was entered from. Not called on the root.
    virtual void leave() = 0;

    /// @param name One component of the current directory, neither `.` nor `..`.
    /// @return What it is, unfollowed; `Missing` where nothing has the name.
    [[nodiscard]] virtual FsResult<EntryKind> inspect(std::string const& name) = 0;

    /// @param name A symbolic link of the current directory.
    /// @return Its target; `Invalid` where @p name is no longer a link, which the walk takes as a
    ///         change under it and looks again.
    [[nodiscard]] virtual FsResult<std::string> readLink(std::string const& name) = 0;

    /// Offers to move from the root to the directory @p components name in one step, as the kernel
    /// can where it resolves beneath a directory itself (`openat2(RESOLVE_BENEATH)`). Only offered
    /// on the root.
    /// @param components Names and `..`, none holding a kernel separator.
    /// @return Whether it took it; what refused the path otherwise (`NotPermitted` for one that
    ///         leaves the root).
    [[nodiscard]] virtual FsResult<Shortcut> shortcut(std::span<std::string const> components) = 0;

    /// Goes back to the root, dropping every directory entered.
    virtual void restart() = 0;
};

/// Where a walk ended: the cursor stands on the directory that holds @c name.
struct WalkEnd
{
    std::string name;                    ///< The final name; empty where the path names that directory.
    EntryKind kind = EntryKind::Missing; ///< What the name is, unfollowed.
    FinalComponent finalComponent = FinalComponent::Any; ///< What the path asked it to be.
};

/// Walks @p plan beneath the cursor's root, following every link before the final component and
/// the final one where @p rules say so, splicing each with @c spliceSymlink.
/// @param cursor Where the walk moves; left on the directory holding the final name.
/// @param plan The planned path.
/// @param rules How links, `..` and the kernel's separators are treated.
/// @return Where it ended; `NotPermitted` where `..` would climb above the root or a link leaves
///         it, `Loop` past @c WalkRules::maxSymlinkHops, `NotDirectory` where the path ends in `/`
///         on something else (or on a link @c TrailingSlashOnLink::Refuse keeps), `Invalid` for a
///         name holding a kernel separator, and what the cursor answers.
[[nodiscard]] CORE_FS_API FsResult<WalkEnd> walkBeneath(IBeneathCursor& cursor,
                                                        PathPlan plan,
                                                        WalkRules const& rules);

} // namespace core::fs::detail
