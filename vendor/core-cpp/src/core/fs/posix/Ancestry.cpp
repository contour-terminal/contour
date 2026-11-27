// SPDX-License-Identifier: Apache-2.0
//
// The real parents of a directory on POSIX, walked through the kernel's own `..`: each one is
// opened relative to the last, never by a name, until `..` is the directory itself -- the root of
// this process's view. The kernel crosses a mount boundary upward on its own, to the directory
// the mount is on; where the mount left shows a subtree of its file system (a bind mount of a
// subdirectory) or below its root (a macOS firmlink), parents are never seen, so each mount left is kept,
// and an `Unrelated` that one of them could hide the other directory behind is refused
// (posix/MountBoundaries.hpp). A walk up never sees a mount beneath either directory, so an
// `Unrelated` also reads the mount table and opens each mount point beneath either one, refusing
// where it may show the other's file system; a parent the user cannot search is the
// error it is, and the caller fails closed. Each parent is opened as a path handle where the system
// has O_PATH, so a directory the user may search but not read is walked through. CMake compiles
// this file on every POSIX system, and windows/Ancestry.cpp on Windows; the one #ifdef below picks a
// flag inside the data, as posix/BeneathDefault.cpp does.
#include <core/fs/detail/Ancestry.hpp>

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/NativeSupport.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/MountBoundaries.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>

namespace core::fs::detail
{

namespace
{

#ifdef O_PATH
    constexpr int ParentAccess = O_PATH;
#else
    constexpr int ParentAccess = O_RDONLY | O_NONBLOCK;
#endif

    constexpr int ParentOpenFlags = ParentAccess | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

    [[nodiscard]] FsResult<ObjectIdentity> identityOf(int handle)
    {
        struct ::stat status {};
        if (::fstat(handle, &status) != 0)
            return errnoError("fstat");
        return ObjectIdentity { .device = static_cast<std::uint64_t>(status.st_dev),
                                .idLow = static_cast<std::uint64_t>(status.st_ino),
                                .idHigh = 0 };
    }

    /// A directory's ancestry: its identity, then each real parent's up to the root, and each mount
    /// the walk left on the way.
    struct Ancestry
    {
        /// A mount the walk left, and the directory it was left from.
        struct Crossing
        {
            MountCrossing crossing;
            ObjectIdentity from;

            [[nodiscard]] bool operator==(Crossing const&) const = default;
        };

        std::vector<ObjectIdentity> chain;
        std::vector<Crossing> crossings;

        [[nodiscard]] bool operator==(Ancestry const&) const = default;
    };

    /// @param handle A directory; never closed here.
    /// @param self Its identity.
    /// @param boundaries What reads each step that leaves a mount.
    /// @param maxDepth How many parents may be walked before `Loop`.
    /// @return Its ancestry.
    [[nodiscard]] FsResult<Ancestry> ancestryOf(int handle,
                                                ObjectIdentity const& self,
                                                IMountBoundaries const& boundaries,
                                                std::size_t maxDepth)
    {
        // Grown per step, never reserved to the cap: a deep tree costs what it walks. At most
        // maxDepth + 1 parents are opened, and the last of them must be the root.
        auto ancestry = Ancestry { .chain = { self }, .crossings = {} };
        auto current = UniqueHandle { platform::InvalidHandle };
        for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, maxDepth + 1))
        {
            auto const from = current.get() == platform::InvalidHandle ? handle : current.get();
            auto parent =
                UniqueHandle { retryOnInterrupt([&] { return ::openat(from, "..", ParentOpenFlags); }) };
            if (parent.get() == platform::InvalidHandle)
                return errnoError("openat(..)");
            auto const identity = identityOf(parent.get());
            if (!identity)
                return std::unexpected { identity.error() };
            if (*identity == ancestry.chain.back())
                return ancestry;
            auto crossed = boundaries.crossing(from, parent.get());
            if (!crossed)
                return std::unexpected { crossed.error() };
            if (*crossed)
                ancestry.crossings.push_back(
                    { .crossing = std::move(**crossed), .from = ancestry.chain.back() });
            ancestry.chain.push_back(*identity);
            current = std::move(parent); // closes the one before
        }
        return refuse(FsErrorCode::Loop);
    }

    /// @return Both directories' ancestries, walked one after the other.
    [[nodiscard]] FsResult<std::pair<Ancestry, Ancestry>> ancestriesOf(int first,
                                                                       ObjectIdentity const& firstSelf,
                                                                       int second,
                                                                       ObjectIdentity const& secondSelf,
                                                                       IMountBoundaries const& boundaries,
                                                                       std::size_t maxDepth)
    {
        return ancestryOf(first, firstSelf, boundaries, maxDepth).and_then([&](Ancestry up) {
            return ancestryOf(second, secondSelf, boundaries, maxDepth).transform([&](Ancestry down) {
                return std::pair { std::move(up), std::move(down) };
            });
        });
    }

    /// @param crossings The mounts one walk left.
    /// @param other The other directory.
    /// @param otherChain The other directory's walked ancestry.
    /// @return Nothing where none of @p crossings can hide @p other; `Unsupported` where one may.
    [[nodiscard]] FsResult<void> checkHidden(std::span<Ancestry::Crossing const> crossings,
                                             int other,
                                             std::span<ObjectIdentity const> otherChain,
                                             IMountBoundaries const& boundaries)
    {
        // A mount of a whole file system of its own, left from its root, hides nothing, and needs no
        // read of the other's mount.
        auto const hidesNothing = [](Ancestry::Crossing const& left) {
            return left.crossing.exit == MountExit::AtRoot && left.crossing.mount.root == "/"
                   && left.crossing.mount.reexposure == Reexposure::Impossible;
        };
        if (std::ranges::all_of(crossings, hidesNothing))
            return {};
        auto const otherMount = boundaries.mountOf(other);
        if (!otherMount)
            return std::unexpected { otherMount.error() };
        auto const hides = [&](Ancestry::Crossing const& left) {
            auto const passes = std::ranges::find(otherChain, left.from) != otherChain.end()
                                    ? PassesThrough::Yes
                                    : PassesThrough::No;
            return mayHide(
                left.crossing,
                HidingCandidate { .mount = *otherMount, .identity = otherChain.front(), .passes = passes });
        };
        if (std::ranges::any_of(crossings, hides))
            return std::unexpected { FsError { .code = FsErrorCode::Unsupported,
                                               .systemCode = 0,
                                               .context =
                                                   "a mount left on the way may hide the other directory" } };
        return {};
    }

    /// @return The identity of @p name in @p directory, a symbolic link's own: a stat needs search
    ///         permission on @p directory alone, so a directory nobody may enter still has one.
    [[nodiscard]] FsResult<ObjectIdentity> identityAt(int directory, std::string const& name)
    {
        struct ::stat status {};
        if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
            return errnoError("fstatat");
        return ObjectIdentity { .device = static_cast<std::uint64_t>(status.st_dev),
                                .idLow = static_cast<std::uint64_t>(status.st_ino),
                                .idHigh = 0 };
    }

    /// @return @p name in @p directory, opened as a directory without following a link.
    [[nodiscard]] FsResult<UniqueHandle> openNamed(int directory, std::string const& name)
    {
        auto opened = UniqueHandle { retryOnInterrupt(
            [&] { return ::openat(directory, name.c_str(), ParentOpenFlags); }) };
        if (opened.get() == platform::InvalidHandle)
            return errnoError("openat(mount point)");
        return opened;
    }

    /// @return A refusal saying a mount beneath a directory may show the other one.
    [[nodiscard]] std::unexpected<FsError> mayExpose(char const* why)
    {
        return std::unexpected { FsError {
            .code = FsErrorCode::Unsupported, .systemCode = 0, .context = why } };
    }

    /// Opens @p row's mount point where it lies strictly beneath @p topSelf, walking its path from
    /// the root one component at a time, never following a link.
    ///
    /// Each component is told apart by identity before it is entered: a path that reaches the
    /// directory a walk up from @p topSelf passed through stays on @p topChain until it reaches
    /// @p topSelf itself, so the first component that is neither is where the path leaves for
    /// somewhere else, and nothing past it needs to be entered -- or may be, where the user cannot.
    /// A component identified is entered only as that directory (@c openNamedAs): a rename between
    /// the stat and the open, which would carry the scan on through a directory it never told
    /// apart, is `Busy`.
    /// @param row A mount of the table.
    /// @param topSelf The directory the mount may lie beneath.
    /// @param topChain Its walked ancestry, itself first and the root last.
    /// @return The mount point, opened; an invalid handle where it is not beneath @p topSelf; or a
    ///         refusal where a component beneath @p topSelf, or the path's shape, could not be read;
    ///         `Busy` where a component changed between its stat and its open.
    [[nodiscard]] FsResult<UniqueHandle> openIfBeneath(MountTableRow const& row,
                                                       ObjectIdentity const& topSelf,
                                                       std::span<ObjectIdentity const> topChain)
    {
        if (!row.path.starts_with('/'))
            return mayExpose("a mount point in the table is no absolute path");
        auto current = UniqueHandle { retryOnInterrupt([] { return ::open("/", ParentOpenFlags); }) };
        if (current.get() == platform::InvalidHandle)
            return errnoError("open(/)");
        auto const root = identityOf(current.get());
        if (!root)
            return std::unexpected { root.error() };
        if (topChain.empty() || *root != topChain.back())
            return mayExpose("the root a mount point is read from is not the walk's");
        auto beneath = *root == topSelf;
        auto components = std::vector<std::string> {};
        for (auto const part: std::string_view { row.path } | std::views::split('/'))
            if (!std::ranges::empty(part))
                components.emplace_back(std::string_view { part });
        if (components.empty())
            return UniqueHandle { platform::InvalidHandle }; // the root is beneath nothing
        for (auto const& component: components)
        {
            if (component == "." || component == "..")
                return mayExpose("a mount point in the table is not a plain path");
            if (beneath)
            {
                auto next = openNamed(current.get(), component);
                if (!next)
                    return std::unexpected { next.error() };
                current = std::move(*next);
                continue;
            }
            auto const identity = identityAt(current.get(), component);
            if (!identity)
                return std::unexpected { identity.error() };
            if (*identity == topSelf)
                beneath = true;
            else if (std::ranges::find(topChain, *identity) == topChain.end())
                return UniqueHandle { platform::InvalidHandle };
            // The mount point itself is the directory, or one of its parents: not beneath it.
            if (&component == &components.back())
                return UniqueHandle { platform::InvalidHandle };
            // Entered only as what was identified: a rename since the stat answers Busy.
            auto next = openNamedAs(current.get(), component, *identity);
            if (!next)
                return std::unexpected { next.error() };
            current = std::move(*next);
        }
        return current;
    }

    /// Refuses where a mount strictly beneath @p topSelf may show @p other's file system: one of
    /// the same file system, one whose file system may show another's, or any where @p other's may,
    /// or where either is unknown.
    /// @param topSelf The directory.
    /// @param topChain Its walked ancestry.
    /// @param other The mount the other directory is on.
    /// @param table The mount table.
    /// @param boundaries What reads a mount.
    /// @return Nothing where no mount beneath @p topSelf may show @p other; `Unsupported` otherwise.
    [[nodiscard]] FsResult<void> checkMountsBeneath(ObjectIdentity const& topSelf,
                                                    std::span<ObjectIdentity const> topChain,
                                                    MountView const& other,
                                                    std::span<MountTableRow const> table,
                                                    IMountBoundaries const& boundaries)
    {
        for (auto const& row: table)
        {
            auto const opened = openIfBeneath(row, topSelf, topChain);
            if (!opened)
                return std::unexpected { opened.error() };
            if (opened->get() == platform::InvalidHandle)
                continue;
            auto const mounted = boundaries.mountOf(opened->get());
            if (!mounted)
                return std::unexpected { mounted.error() };
            // A path that opens onto another mount than the row's (one made over it, or a row of the
            // table that moved) says nothing about the row's.
            if (!row.mount || mounted->mount != row.mount)
                return mayExpose("a mount point beneath the directory opens onto another mount");
            auto const shows = mounted->reexposure == Reexposure::Possible
                               || other.reexposure == Reexposure::Possible || !mounted->fileSystem
                               || !other.fileSystem || *mounted->fileSystem == *other.fileSystem;
            if (shows)
                return mayExpose("a mount beneath one directory may show the other's file system");
        }
        return {};
    }

    /// Refuses where a mount beneath either directory may show the other's file system; `Busy` where
    /// the mount table changed while it was scanned.
    [[nodiscard]] FsResult<void> checkBothBeneath(
        int first, Ancestry const& up, int second, Ancestry const& down, IMountBoundaries const& boundaries)
    {
        auto const table = boundaries.mountTable();
        if (!table)
            return std::unexpected { table.error() };
        auto const firstMount = boundaries.mountOf(first);
        if (!firstMount)
            return std::unexpected { firstMount.error() };
        auto const secondMount = boundaries.mountOf(second);
        if (!secondMount)
            return std::unexpected { secondMount.error() };
        if (auto scanned = checkMountsBeneath(up.chain.front(), up.chain, *secondMount, *table, boundaries)
                               .and_then([&] {
                                   return checkMountsBeneath(
                                       down.chain.front(), down.chain, *firstMount, *table, boundaries);
                               });
            !scanned)
            return scanned;
        auto const again = boundaries.mountTable();
        if (!again || *again != *table)
            return refuse(FsErrorCode::Busy);
        return {};
    }

    /// @return Whether @p ancestor is a path above @p path, `/` above every other.
    [[nodiscard]] bool isAbove(std::string_view ancestor, std::string_view path) noexcept
    {
        if (ancestor == path)
            return false;
        if (ancestor == "/")
            return true;
        return path.starts_with(ancestor) && path.size() > ancestor.size() && path[ancestor.size()] == '/';
    }

} // namespace

FsResult<UniqueHandle> openNamedAs(int directory, std::string const& name, ObjectIdentity const& expected)
{
    return openNamed(directory, name).and_then([&](UniqueHandle opened) -> FsResult<UniqueHandle> {
        auto const identity = identityOf(opened.get());
        if (!identity)
            return std::unexpected { identity.error() };
        if (*identity != expected)
            return refuse(FsErrorCode::Busy);
        return opened;
    });
}

bool mayHide(MountCrossing const& left, HidingCandidate const& other)
{
    auto const& mount = left.mount;
    auto const sameMount = mount.mount && other.mount.mount && *mount.mount == *other.mount.mount;
    // Below its root (a firmlink), the step skipped the mount's own directories above the one left
    // from and each directory on the mount point's path; a directory whose walk passed the one left
    // from lies at or beneath it, and is none of them. This comes before every shortcut: a skipped
    // directory on the mount point's path is on another mount, often another device.
    if (left.exit == MountExit::BelowRoot && other.passes == PassesThrough::No)
    {
        auto const skipped = std::ranges::find(left.skipped, other.identity) != left.skipped.end();
        switch (left.skippedSet)
        {
            case SkippedSet::Unknown: return true;
            case SkippedSet::AtLeast:
                if (skipped || sameMount)
                    return true;
                break;
            case SkippedSet::Exactly:
                if (skipped)
                    return true;
                break;
        }
    }
    // On the mount left, the other walk is whole inside it.
    if (sameMount)
        return false;
    if (mount.reexposure == Reexposure::Possible)
        return true;
    if (left.exit == MountExit::AtRoot && mount.root == "/")
        return false;
    if (other.mount.reexposure == Reexposure::Possible)
        return true;
    if (!mount.fileSystem || !other.mount.fileSystem)
        return true;
    if (*mount.fileSystem != *other.mount.fileSystem)
        return false;
    // Below the root, through another mount of the same file system: decided only where every
    // directory skipped is known by identity, which the check above has compared.
    if (left.exit == MountExit::BelowRoot)
        return other.passes == PassesThrough::No && left.skippedSet != SkippedSet::Exactly;
    // One file system, reached through another mount: a directory there lies beneath what that
    // mount shows, so it can be above left's root only where what that mount shows is.
    return !mount.root || !other.mount.root || isAbove(*other.mount.root, *mount.root);
}

Reexposure reexposureOf(std::span<ReexposingType const> table, std::string_view type)
{
    if (type.empty())
        return Reexposure::Possible;
    auto const matches = [&](ReexposingType const& row) {
        return row.match == TypeMatch::Family ? type.starts_with(row.name) : type == row.name;
    };
    return std::ranges::any_of(table, matches) ? Reexposure::Possible : Reexposure::Impossible;
}

FsResult<Relation> nativeRelation(NativeContext const& context,
                                  platform::NativeHandle first,
                                  platform::NativeHandle second)
{
    if (context.mountBoundaries == nullptr)
        return refuse(FsErrorCode::Unsupported);
    auto const& boundaries = *context.mountBoundaries;
    auto const maxDepth = context.options.maxAncestorDepth;
    return identityOf(first).and_then([&](ObjectIdentity const& firstSelf) {
        return identityOf(second).and_then([&](ObjectIdentity const& secondSelf) -> FsResult<Relation> {
            if (firstSelf == secondSelf)
                return Relation::Same;
            // Each `..` is atomic, but the two walks are not one instant: a directory moved between
            // them, or during one, could make the chains agree with no tree that ever existed. Both are
            // walked again, and any difference is Busy. What remains is a move away and back between
            // the two passes (ABA), which leaves each chain as a tree that did exist.
            auto const walked = ancestriesOf(first, firstSelf, second, secondSelf, boundaries, maxDepth);
            if (!walked)
                return std::unexpected { walked.error() };
            auto const again = ancestriesOf(first, firstSelf, second, secondSelf, boundaries, maxDepth);
            if (!again || *again != *walked)
                return refuse(FsErrorCode::Busy);
            auto const& [up, down] = *walked;
            auto const decided = decideRelation(up.chain, down.chain);
            // A containment the walks met is real; an `Unrelated` stands only where no mount left on
            // either way hides the other directory above its root, and no mount beneath either
            // shows the other's file system.
            if (decided != Relation::Unrelated)
                return decided;
            return checkHidden(up.crossings, second, down.chain, boundaries)
                .and_then([&] { return checkHidden(down.crossings, first, up.chain, boundaries); })
                .and_then([&] { return checkBothBeneath(first, up, second, down, boundaries); })
                .transform([] { return Relation::Unrelated; });
        });
    });
}

} // namespace core::fs::detail
