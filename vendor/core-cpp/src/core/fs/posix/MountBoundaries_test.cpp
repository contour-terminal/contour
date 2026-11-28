// SPDX-License-Identifier: Apache-2.0
//
// What a POSIX relation walk decides from the mounts it leaves (mayHide, the table every system's
// boundaries feed), and the walk itself under scripted boundaries: a mount left that may hide the
// other directory turns `Unrelated` into `Unsupported`, and a rename between the two passes is
// `Busy`.
#include <core/async/testing/ManualExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/Ancestry.hpp>
#include <core/fs/detail/NativeSupport.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/MountBoundaries.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>

using core::fs::FsErrorCode;
using core::fs::FsResult;
using core::fs::Relation;
using core::fs::detail::HidingCandidate;
using core::fs::detail::IMountBoundaries;
using core::fs::detail::mayHide;
using core::fs::detail::MountCrossing;
using core::fs::detail::MountExit;
using core::fs::detail::MountTableRow;
using core::fs::detail::MountView;
using core::fs::detail::ObjectIdentity;
using core::fs::detail::openNamedAs;
using core::fs::detail::PassesThrough;
using core::fs::detail::Reexposure;
using core::fs::detail::SkippedSet;
using core::fs::detail::UniqueHandle;

namespace
{

/// One row of the hiding table: a mount left, the other directory's mount, whether the other walk
/// passed the directory left from, the answer, and the other directory's identity.
struct HidingRow
{
    char const* name;
    MountCrossing left;
    MountView other;
    PassesThrough passes;
    bool hides;
    ObjectIdentity identity;
};

/// Identities of a scripted macOS: /System and /System/Volumes on the system volume (device 8),
/// the Data volume's root (device 7), and a directory that is none of them.
constexpr auto SystemDirectory = ObjectIdentity { .device = 8, .idLow = 10, .idHigh = 0 };
constexpr auto VolumesDirectory = ObjectIdentity { .device = 8, .idLow = 11, .idHigh = 0 };
constexpr auto DataRoot = ObjectIdentity { .device = 7, .idLow = 2, .idHigh = 0 };
constexpr auto Elsewhere = ObjectIdentity { .device = 99, .idLow = 99, .idHigh = 0 };

[[nodiscard]] MountView linuxMount(char const* id,
                                   std::uint64_t fileSystem,
                                   char const* root,
                                   Reexposure reexposure = Reexposure::Impossible)
{
    return MountView { .mount = id, .fileSystem = fileSystem, .root = root, .reexposure = reexposure };
}

[[nodiscard]] MountView bsdMount(char const* name,
                                 std::uint64_t fileSystem,
                                 Reexposure reexposure = Reexposure::Impossible)
{
    return MountView {
        .mount = name, .fileSystem = fileSystem, .root = std::nullopt, .reexposure = reexposure
    };
}

[[nodiscard]] MountCrossing atRoot(MountView mount)
{
    return MountCrossing {
        .mount = std::move(mount), .exit = MountExit::AtRoot, .skipped = {}, .skippedSet = SkippedSet::Unknown
    };
}

/// @return A crossing below @p mount's root that skipped @p skipped, known as far as @p known says.
[[nodiscard]] MountCrossing belowRoot(MountView mount, std::vector<ObjectIdentity> skipped, SkippedSet known)
{
    return MountCrossing { .mount = std::move(mount),
                           .exit = MountExit::BelowRoot,
                           .skipped = std::move(skipped),
                           .skippedSet = known };
}

/// @return A firmlink crossing of the Data volume whose skipped directories are all known.
[[nodiscard]] MountCrossing firmlink(MountView data, SkippedSet known = SkippedSet::Exactly)
{
    return belowRoot(std::move(data), { SystemDirectory, VolumesDirectory, DataRoot }, known);
}

/// A directory's device and inode.
using DirectoryIdentity = std::pair<std::uint64_t, std::uint64_t>;

/// Boundaries a test scripts: every step stays on one mount but the one from @c crossingAt, which
/// leaves @c left; every directory is on @c others but those @c mountsAt names; @c onStep runs at
/// each step, numbered from zero, before it is answered; and @c table answers each read of the mount
/// table, numbered from zero (none: an empty table).
class ScriptedBoundaries final: public IMountBoundaries
{
  public:
    struct Script
    {
        std::optional<DirectoryIdentity> crossingAt;
        MountCrossing left;
        MountView others;
        std::function<void(std::size_t)> onStep;
        std::function<FsResult<std::vector<MountTableRow>>(std::size_t)> table;
        std::vector<std::pair<DirectoryIdentity, MountView>> mountsAt;
    };

    explicit ScriptedBoundaries(Script script): _script { std::move(script) } {}

    [[nodiscard]] FsResult<std::optional<MountCrossing>> crossing(int child, int /*parent*/) const override
    {
        if (_script.onStep)
            _script.onStep(_steps);
        ++_steps;
        struct ::stat status {};
        REQUIRE(::fstat(child, &status) == 0);
        auto const here = std::pair { static_cast<std::uint64_t>(status.st_dev),
                                      static_cast<std::uint64_t>(status.st_ino) };
        if (_script.crossingAt == here)
            return std::optional { _script.left };
        return std::nullopt;
    }

    [[nodiscard]] FsResult<MountView> mountOf(int handle) const override
    {
        struct ::stat status {};
        REQUIRE(::fstat(handle, &status) == 0);
        auto const here = DirectoryIdentity { static_cast<std::uint64_t>(status.st_dev),
                                              static_cast<std::uint64_t>(status.st_ino) };
        auto const named =
            std::ranges::find(_script.mountsAt, here, [](auto const& row) { return row.first; });
        return named != _script.mountsAt.end() ? named->second : _script.others;
    }

    [[nodiscard]] FsResult<std::vector<MountTableRow>> mountTable() const override
    {
        auto const read = _tableReads++;
        return _script.table ? _script.table(read) : std::vector<MountTableRow> {};
    }

    /// @return How many steps have been checked.
    [[nodiscard]] std::size_t steps() const noexcept { return _steps; }

  private:
    Script _script;
    mutable std::size_t _steps = 0;
    mutable std::size_t _tableReads = 0;
};

/// A native file system's context with @p boundaries, and what it is built from.
class ScriptedContext
{
  public:
    explicit ScriptedContext(std::shared_ptr<ScriptedBoundaries const> boundaries):
        _context { _home, _blocking, _clock,  _random, {},
                   "/",   nullptr,   nullptr, nullptr, std::move(boundaries) }
    {
    }

    /// @return How @p first and @p second relate, walked natively.
    [[nodiscard]] FsResult<Relation> relation(std::filesystem::path const& first,
                                              std::filesystem::path const& second) const
    {
        auto const a = openDirectory(first);
        auto const b = openDirectory(second);
        return core::fs::detail::nativeRelation(_context, a.get(), b.get());
    }

  private:
    [[nodiscard]] static UniqueHandle openDirectory(std::filesystem::path const& path)
    {
        auto handle = UniqueHandle { ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
        REQUIRE(handle.get() >= 0);
        return handle;
    }

    core::async::testing::ManualExecutor _home;
    core::async::testing::ManualExecutor _blocking;
    core::platform::ManualWallClock _clock;
    core::fs::test::FixedRandom _random { 1 };
    core::fs::detail::NativeContext _context;
};

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> identityOf(std::filesystem::path const& path)
{
    struct ::stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    return { static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino) };
}

[[nodiscard]] ObjectIdentity objectIdentityOf(std::filesystem::path const& path)
{
    auto const [device, inode] = identityOf(path);
    return ObjectIdentity { .device = device, .idLow = inode, .idHigh = 0 };
}

[[nodiscard]] bool isError(FsResult<Relation> const& answer, FsErrorCode code)
{
    return !answer.has_value() && answer.error().code == code;
}

} // namespace

TEST_CASE("mayHide: what a mount left can hide", "[fs][relation]")
{
    using enum PassesThrough;
    auto const overlay = Reexposure::Possible;
    auto const rows = std::to_array<HidingRow>({
        // Linux: mount ids, the superblock device, the root column; `..` leaves only at a root.
        { "a whole file system hides nothing",
          atRoot(linuxMount("35", 1, "/")),
          linuxMount("22", 1, "/"),
          No,
          false,
          Elsewhere },
        { "a sub-bind hides the mount of its whole file system",
          atRoot(linuxMount("61", 1, "/a/sub")),
          linuxMount("22", 1, "/"),
          No,
          true,
          Elsewhere },
        { "a sub-bind hides nothing from a directory on itself",
          atRoot(linuxMount("61", 1, "/a/sub")),
          linuxMount("61", 1, "/a/sub"),
          No,
          false,
          Elsewhere },
        { "a sub-bind hides nothing from another file system",
          atRoot(linuxMount("61", 1, "/a/sub")),
          linuxMount("35", 2, "/"),
          No,
          false,
          Elsewhere },
        { "a subvolume hides nothing from a sibling subvolume",
          atRoot(linuxMount("69", 3, "/home")),
          linuxMount("70", 3, "/root"),
          No,
          false,
          Elsewhere },
        { "a nested subvolume is hidden by its parent's mount",
          atRoot(linuxMount("71", 3, "/root/var")),
          linuxMount("70", 3, "/root"),
          No,
          true,
          Elsewhere },
        { "a second bind of the same subtree is not above it",
          atRoot(linuxMount("72", 1, "/a/sub")),
          linuxMount("61", 1, "/a/sub"),
          No,
          false,
          Elsewhere },
        { "a bind of a subtree below is not above it",
          atRoot(linuxMount("61", 1, "/a/sub")),
          linuxMount("73", 1, "/a/sub/deeper"),
          No,
          false,
          Elsewhere },
        { "a path sharing only a prefix of letters is not above",
          atRoot(linuxMount("61", 1, "/a/sub")),
          linuxMount("74", 1, "/a/su"),
          No,
          false,
          Elsewhere },
        { "an unknown root hides",
          atRoot(
              MountView { .mount = "61", .fileSystem = 1, .root = {}, .reexposure = Reexposure::Impossible }),
          linuxMount("22", 1, "/"),
          No,
          true,
          Elsewhere },
        { "an unknown file system hides",
          atRoot(MountView {
              .mount = "61", .fileSystem = {}, .root = {}, .reexposure = Reexposure::Impossible }),
          linuxMount("22", 1, "/"),
          No,
          true,
          Elsewhere },
        // A file system that may re-expose another's: only its own mount decides.
        { "an overlay left hides from another file system",
          atRoot(linuxMount("80", 5, "/", overlay)),
          linuxMount("22", 1, "/"),
          No,
          true,
          Elsewhere },
        { "an overlay left hides nothing from a directory on itself",
          atRoot(linuxMount("80", 5, "/", overlay)),
          linuxMount("80", 5, "/", overlay),
          No,
          false,
          Elsewhere },
        { "a FUSE mount left hides from another file system",
          atRoot(linuxMount("81", 6, "/", overlay)),
          linuxMount("35", 2, "/"),
          No,
          true,
          Elsewhere },
        { "a directory on an overlay may be what a sub-bind hides",
          atRoot(linuxMount("61", 1, "/a/sub")),
          linuxMount("80", 5, "/", overlay),
          No,
          true,
          Elsewhere },
        { "a whole file system hides nothing, even from an overlay",
          atRoot(linuxMount("35", 2, "/")),
          linuxMount("80", 5, "/", overlay),
          No,
          false,
          Elsewhere },
        // macOS and the BSDs: no root is known, so only the mount or the file system decides.
        { "BSD: the same mount",
          atRoot(bsdMount("1:2:/Volumes/Data", 7)),
          bsdMount("1:2:/Volumes/Data", 7),
          No,
          false,
          Elsewhere },
        { "BSD: another file system",
          atRoot(bsdMount("1:2:/Volumes/Data", 7)),
          bsdMount("3:4:/", 8),
          No,
          false,
          Elsewhere },
        { "BSD: one file system through another mount",
          atRoot(bsdMount("1:2:/mnt/a", 7)),
          bsdMount("3:4:/", 7),
          No,
          true,
          Elsewhere },
        { "BSD: a nullfs mount and anything else",
          atRoot(bsdMount("5:6:/mnt/null", 9, overlay)),
          bsdMount("3:4:/", 8),
          No,
          true,
          Elsewhere },
        { "BSD: a nullfs mount and itself",
          atRoot(bsdMount("5:6:/mnt/null", 9, overlay)),
          bsdMount("5:6:/mnt/null", 9, overlay),
          No,
          false,
          Elsewhere },
        // A macOS firmlink: the Data volume left at /Users, below its root, skipping /System,
        // /System/Volumes and the Data root.
        { "firmlink: the Data root, not passed",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7)),
          bsdMount("1:2:/System/Volumes/Data", 7),
          No,
          true,
          DataRoot },
        { "firmlink: a directory beneath the firmlink",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7)),
          bsdMount("1:2:/System/Volumes/Data", 7),
          Yes,
          false,
          Elsewhere },
        { "firmlink: another directory on the Data volume, all skipped known",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7)),
          bsdMount("1:2:/System/Volumes/Data", 7),
          No,
          false,
          Elsewhere },
        { "firmlink: another directory on the Data volume, more skipped than known",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7), SkippedSet::AtLeast),
          bsdMount("1:2:/System/Volumes/Data", 7),
          No,
          true,
          Elsewhere },
        { "firmlink: /System/Volumes, on the parent mount and another device",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7)),
          bsdMount("3:4:/", 8),
          No,
          true,
          VolumesDirectory },
        { "firmlink: /System, on the parent mount and another device",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7)),
          bsdMount("3:4:/", 8),
          No,
          true,
          SystemDirectory },
        { "firmlink: an unrelated directory on the parent mount",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7)),
          bsdMount("3:4:/", 8),
          No,
          false,
          Elsewhere },
        { "firmlink: a mount point's path that could not be read",
          belowRoot(bsdMount("1:2:/System/Volumes/Data", 7), {}, SkippedSet::Unknown),
          bsdMount("3:4:/", 8),
          No,
          true,
          Elsewhere },
        { "firmlink: the same file system through another mount, more skipped than known",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7), SkippedSet::AtLeast),
          bsdMount("3:4:/Volumes/Again", 7),
          No,
          true,
          Elsewhere },
        { "firmlink: the same file system through another mount, all skipped known",
          firmlink(bsdMount("1:2:/System/Volumes/Data", 7)),
          bsdMount("3:4:/Volumes/Again", 7),
          No,
          false,
          Elsewhere },
        { "below the root of a whole file system on Linux terms",
          belowRoot(linuxMount("35", 2, "/"), {}, SkippedSet::Unknown),
          linuxMount("22", 2, "/"),
          No,
          true,
          Elsewhere },
        // A system that names no mount and no type: the device alone, and it fails closed.
        { "device only: another device",
          belowRoot(MountView { .mount = {}, .fileSystem = 1, .root = {}, .reexposure = overlay },
                    {},
                    SkippedSet::Unknown),
          MountView { .mount = {}, .fileSystem = 2, .root = {}, .reexposure = overlay },
          No,
          true,
          Elsewhere },
    });
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(mayHide(row.left,
                      HidingCandidate { .mount = row.other, .identity = row.identity, .passes = row.passes })
              == row.hides);
    }
}

TEST_CASE("reexposureOf: the types that may show another file system", "[fs][relation]")
{
    using core::fs::detail::ReexposingType;
    using core::fs::detail::reexposureOf;
    using core::fs::detail::TypeMatch;
    constexpr auto Table = std::to_array<ReexposingType>({
        { .name = "overlay", .match = TypeMatch::Exact },
        { .name = "fuse", .match = TypeMatch::Family },
    });
    CHECK(reexposureOf(Table, "overlay") == Reexposure::Possible);
    CHECK(reexposureOf(Table, "overlayfs") == Reexposure::Impossible); // exact, not a prefix
    CHECK(reexposureOf(Table, "fuse") == Reexposure::Possible);
    CHECK(reexposureOf(Table, "fuse.sshfs") == Reexposure::Possible);
    CHECK(reexposureOf(Table, "fuseblk") == Reexposure::Possible);
    CHECK(reexposureOf(Table, "ext4") == Reexposure::Impossible);
    CHECK(reexposureOf(Table, "") == Reexposure::Possible); // unreadable
}

TEST_CASE("relation: a mount left that may hide the other directory is Unsupported", "[fs][native][relation]")
{
    // m is scripted as a mount the walk from m/key leaves; p lies beside m, on the mount the script
    // names. The walks meet no containment, so the mount decides.
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "m" / "key");
    std::filesystem::create_directories(host / "p");
    struct Row
    {
        char const* name;
        MountCrossing left;
        MountView others;
        std::optional<FsErrorCode> refusal;
    };
    auto const rows = std::to_array<Row>({
        { "p on the mount left", atRoot(bsdMount("1:2:/m", 7)), bsdMount("1:2:/m", 7), std::nullopt },
        { "p on another file system", atRoot(bsdMount("1:2:/m", 7)), bsdMount("3:4:/", 8), std::nullopt },
        { "p on the same file system through another mount",
          atRoot(bsdMount("1:2:/m", 7)),
          bsdMount("3:4:/", 7),
          FsErrorCode::Unsupported },
        { "m an overlay, p on another file system",
          atRoot(linuxMount("80", 5, "/", Reexposure::Possible)),
          linuxMount("22", 1, "/"),
          FsErrorCode::Unsupported },
        { "m a FUSE mount, p on another file system",
          atRoot(bsdMount("7:8:/m", 9, Reexposure::Possible)),
          bsdMount("3:4:/", 8),
          FsErrorCode::Unsupported },
    });
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const context = ScriptedContext { std::make_shared<ScriptedBoundaries const>(
            ScriptedBoundaries::Script { .crossingAt = identityOf(host / "m"),
                                         .left = row.left,
                                         .others = row.others,
                                         .onStep = {},
                                         .table = {},
                                         .mountsAt = {} }) };
        auto const keyFirst = context.relation(host / "m" / "key", host / "p");
        auto const keySecond = context.relation(host / "p", host / "m" / "key");
        if (row.refusal)
        {
            CHECK(isError(keyFirst, *row.refusal));
            CHECK(isError(keySecond, *row.refusal));
        }
        else
        {
            CHECK((keyFirst && *keyFirst == Relation::Unrelated));
            CHECK((keySecond && *keySecond == Relation::Unrelated));
        }
        // A containment the walks meet stands, whatever the mount.
        auto const contained = context.relation(host / "m", host / "m" / "key");
        CHECK((contained && *contained == Relation::FirstContainsSecond));
    }
}

TEST_CASE("relation: a mount left below its root, as at a macOS firmlink", "[fs][native][relation]")
{
    // sys/vol/d stands for /System/Volumes/Data, d for the Data volume's root, and u for /Users. The
    // host's real `..` cannot skip a parent, so u lies beside them, as the system volume's /Users
    // does: the walk from u/me leaves d's mount at u, below its root, and skips sys, sys/vol and d.
    // Every directory is scripted on the mount a row names.
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    for (auto const* directory: { "sys/vol/d", "u/me", "u/foo", "elsewhere" })
        std::filesystem::create_directories(host / directory);
    auto const data = bsdMount("1:2:/System/Volumes/Data", 7);
    auto const parentMount = bsdMount("3:4:/", 8);
    auto const skipped = std::vector { objectIdentityOf(host / "sys"),
                                       objectIdentityOf(host / "sys" / "vol"),
                                       objectIdentityOf(host / "sys" / "vol" / "d") };
    auto const scripted = [&](SkippedSet known, MountView const& others) {
        return std::make_shared<ScriptedBoundaries const>(
            ScriptedBoundaries::Script { .crossingAt = identityOf(host / "u"),
                                         .left = belowRoot(data, skipped, known),
                                         .others = others,
                                         .onStep = {},
                                         .table = {},
                                         .mountsAt = {} });
    };
    struct Row
    {
        char const* name;
        SkippedSet known;
        MountView const* others;
        char const* grant;
        std::optional<FsErrorCode> refusal;
    };
    auto const rows = std::to_array<Row>({
        { "the Data root", SkippedSet::Exactly, &data, "sys/vol/d", FsErrorCode::Unsupported },
        { "another directory on the Data volume, all skipped known",
          SkippedSet::Exactly,
          &data,
          "elsewhere",
          std::nullopt },
        { "another directory on the Data volume, more skipped than known",
          SkippedSet::AtLeast,
          &data,
          "elsewhere",
          FsErrorCode::Unsupported },
        { "a directory on the mount point's path, on the parent mount",
          SkippedSet::Exactly,
          &parentMount,
          "sys/vol",
          FsErrorCode::Unsupported },
        { "the top of the mount point's path, on the parent mount",
          SkippedSet::Exactly,
          &parentMount,
          "sys",
          FsErrorCode::Unsupported },
        { "an unrelated directory on the parent mount",
          SkippedSet::Exactly,
          &parentMount,
          "elsewhere",
          std::nullopt },
        { "an unrelated directory, the mount point's path unread",
          SkippedSet::Unknown,
          &parentMount,
          "elsewhere",
          FsErrorCode::Unsupported },
    });
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const context = ScriptedContext { scripted(row.known, *row.others) };
        auto const grantFirst = context.relation(host / row.grant, host / "u" / "me");
        auto const keyFirst = context.relation(host / "u" / "me", host / row.grant);
        if (row.refusal)
        {
            CHECK(isError(grantFirst, *row.refusal));
            CHECK(isError(keyFirst, *row.refusal));
        }
        else
        {
            CHECK((grantFirst && *grantFirst == Relation::Unrelated));
            CHECK((keyFirst && *keyFirst == Relation::Unrelated));
        }
        // Both beneath the firmlink: both walks pass it, so the answer stands whatever is known.
        auto const siblings = context.relation(host / "u" / "foo", host / "u" / "me");
        CHECK((siblings && *siblings == Relation::Unrelated));
    }
}

TEST_CASE("relation: a move between the two passes is Busy", "[fs][native][relation]")
{
    // x/a and y are unrelated in the first pass. At the first step of the second, x moves into y, so
    // the second pass walks a up through y: the passes differ, and the answer is Busy, never the
    // first pass's Unrelated.
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "x" / "a");
    std::filesystem::create_directories(host / "y");

    // A pass is as many steps as both chains are long below the root: counted once, unscripted.
    auto const counter = std::make_shared<ScriptedBoundaries const>(ScriptedBoundaries::Script {});
    auto const unscripted = ScriptedContext { counter }.relation(host / "x" / "a", host / "y");
    REQUIRE((unscripted && *unscripted == Relation::Unrelated));
    auto const pass = counter->steps() / 2;
    REQUIRE(pass > 0);

    auto moved = false;
    auto const mover = std::make_shared<ScriptedBoundaries const>(
        ScriptedBoundaries::Script { .crossingAt = std::nullopt,
                                     .left = {},
                                     .others = {},
                                     .onStep =
                                         [&](std::size_t step) {
                                             if (step != pass)
                                                 return;
                                             std::filesystem::rename(host / "x", host / "y" / "x");
                                             moved = true;
                                         },
                                     .table = {},
                                     .mountsAt = {} });
    auto const answer = ScriptedContext { mover }.relation(host / "x" / "a", host / "y");
    REQUIRE(moved);
    CHECK(isError(answer, FsErrorCode::Busy));
}

TEST_CASE("relation across this system's own mounts", "[fs][native][relation]")
{
    // /dev is a mount of its own on Linux (devtmpfs), macOS and the BSDs (devfs): a walk leaves it
    // for the root, and the fixture's directory is on another file system, which nothing /dev
    // could hide is on. With the native boundaries -- statfs on macOS and the BSDs, mountinfo on
    // Linux -- that is Unrelated, never a refusal; and the root contains /dev.
    auto const devStatus = [] {
        struct ::stat status {};
        return ::stat("/dev", &status) == 0 ? std::optional { status.st_dev } : std::nullopt;
    }();
    auto const rootStatus = [] {
        struct ::stat status {};
        return ::stat("/", &status) == 0 ? std::optional { status.st_dev } : std::nullopt;
    }();
    if (!devStatus || !rootStatus || *devStatus == *rootStatus)
        SKIP("/dev is not a mount of its own here");
    auto fixture = core::fs::test::DescriptorFixture::createNative();
    auto& system = fixture.fileSystem();
    auto const dev = fixture.run(system.openPreopen("/dev", core::fs::DescriptorFlags::Read));
    auto const root = fixture.run(system.openPreopen("/", core::fs::DescriptorFlags::Read));
    REQUIRE(dev.has_value());
    REQUIRE(root.has_value());
    auto const fromDev = fixture.run(system.relation(**dev, fixture.root()));
    INFO((fromDev ? std::string { "decided" } : fromDev.error().toString()));
    CHECK((fromDev && *fromDev == Relation::Unrelated));
    auto const fromRoot = fixture.run(system.relation(**root, **dev));
    CHECK((fromRoot && *fromRoot == Relation::FirstContainsSecond));
}

TEST_CASE("relation: a mount beneath either directory that may show the other's file system",
          "[fs][native][relation]")
{
    // g and k/secret lie side by side on one file system, so neither walk up meets the other. The
    // table names a mount at g/inner -- beneath g -- and the scan opens it: where its file system is
    // the key's, or may show another's, the key may be reached through g, and Unrelated cannot stand.
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "g" / "inner" / "deeper");
    std::filesystem::create_directories(host / "k" / "secret");
    auto const keyFileSystem = bsdMount("1:2:/", 7);
    struct Row
    {
        char const* name;
        char const* mountPoint;               ///< Beneath the host, where the table says it is.
        std::optional<std::string> tableName; ///< The name the table gives it.
        MountView mounted;                    ///< What opening it finds.
        std::optional<FsErrorCode> refusal;
    };
    auto const rows = std::to_array<Row>({
        { "the key's file system beneath the grant",
          "g/inner",
          "1:2:/g/inner",
          bsdMount("1:2:/g/inner", 7),
          FsErrorCode::Unsupported },
        { "a file system that may show another's beneath the grant",
          "g/inner",
          "5:6:/g/inner",
          bsdMount("5:6:/g/inner", 9, Reexposure::Possible),
          FsErrorCode::Unsupported },
        { "a file system of its own beneath the grant",
          "g/inner",
          "5:6:/g/inner",
          bsdMount("5:6:/g/inner", 9),
          std::nullopt },
        { "the key's file system deeper beneath the grant",
          "g/inner/deeper",
          "1:2:/g/inner/deeper",
          bsdMount("1:2:/g/inner/deeper", 7),
          FsErrorCode::Unsupported },
        { "a path that opens onto another mount than the table's",
          "g/inner",
          "8:8:/g/inner",
          bsdMount("1:2:/g/inner", 9),
          FsErrorCode::Unsupported },
        { "the key's file system beside the grant", "k", "1:2:/k", bsdMount("1:2:/k", 7), std::nullopt },
    });
    for (auto const& row: rows)
    {
        INFO(row.name);
        // As a mount table names it: the path with no link in it (macOS's /var is one).
        auto const mountPoint = std::filesystem::canonical(host / row.mountPoint).string();
        auto const context =
            ScriptedContext { std::make_shared<ScriptedBoundaries const>(ScriptedBoundaries::Script {
                .crossingAt = std::nullopt,
                .left = {},
                .others = keyFileSystem,
                .onStep = {},
                .table =
                    [&](std::size_t) {
                        return std::vector { MountTableRow { .path = mountPoint, .mount = row.tableName } };
                    },
                .mountsAt = { { identityOf(host / row.mountPoint), row.mounted } } }) };
        auto const grantFirst = context.relation(host / "g", host / "k" / "secret");
        auto const keyFirst = context.relation(host / "k" / "secret", host / "g");
        if (row.refusal)
        {
            CHECK(isError(grantFirst, *row.refusal));
            CHECK(isError(keyFirst, *row.refusal));
        }
        else
        {
            CHECK((grantFirst && *grantFirst == Relation::Unrelated));
            CHECK((keyFirst && *keyFirst == Relation::Unrelated));
        }
    }
}

TEST_CASE("relation: a mount point beneath a directory that cannot be read is Unsupported",
          "[fs][native][relation]")
{
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "g");
    std::filesystem::create_directories(host / "k");
    auto const missing = (std::filesystem::canonical(host / "g") / "gone").string();
    auto const context =
        ScriptedContext { std::make_shared<ScriptedBoundaries const>(ScriptedBoundaries::Script {
            .crossingAt = std::nullopt,
            .left = {},
            .others = bsdMount("1:2:/", 7),
            .onStep = {},
            .table =
                [&](std::size_t) {
                    return std::vector { MountTableRow { .path = missing, .mount = "1:2:/g/gone" } };
                },
            .mountsAt = {} }) };
    auto const answer = context.relation(host / "g", host / "k");
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().code != FsErrorCode::Busy);
}

TEST_CASE("the beneath scan opens a component only as the directory it identified", "[fs][native][relation]")
{
    // The scan identifies a component with fstatat and then opens it by name. A rename in between
    // makes the name another directory's: the opened handle's identity is not the one checked, and
    // the answer is Busy rather than a scan continued through a directory it never identified.
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "x");
    auto const parent = UniqueHandle { ::open(host.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    REQUIRE(parent.get() >= 0);
    auto const identified = objectIdentityOf(host / "x");

    SECTION("the name still names the directory identified")
    {
        auto const opened = openNamedAs(parent.get(), "x", identified);
        REQUIRE(opened.has_value());
        CHECK(objectIdentityOf(host / "x") == identified);
    }

    SECTION("the name was moved onto another directory in between")
    {
        std::filesystem::rename(host / "x", host / "moved");
        std::filesystem::create_directories(host / "x");
        auto const opened = openNamedAs(parent.get(), "x", identified);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == FsErrorCode::Busy);
    }
}

TEST_CASE("relation: a mount table that changes while it is scanned is Busy", "[fs][native][relation]")
{
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "g");
    std::filesystem::create_directories(host / "k");
    auto const context =
        ScriptedContext { std::make_shared<ScriptedBoundaries const>(ScriptedBoundaries::Script {
            .crossingAt = std::nullopt,
            .left = {},
            .others = bsdMount("1:2:/", 7),
            .onStep = {},
            .table = [](std::size_t read) -> FsResult<std::vector<MountTableRow>> {
                // The first read sees nothing; a mount arrives before the read that confirms it.
                auto rows = std::vector<MountTableRow> {};
                if (read > 0)
                    rows.push_back(MountTableRow { .path = "/nowhere", .mount = "9:9:/nowhere" });
                return rows;
            },
            .mountsAt = {} }) };
    CHECK(isError(context.relation(host / "g", host / "k"), FsErrorCode::Busy));
}

TEST_CASE("relation: a mount table that cannot be read leaves no Unrelated standing",
          "[fs][native][relation]")
{
    auto const fixture = core::fs::test::DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "g");
    std::filesystem::create_directories(host / "k");
    auto const context =
        ScriptedContext { std::make_shared<ScriptedBoundaries const>(ScriptedBoundaries::Script {
            .crossingAt = std::nullopt,
            .left = {},
            .others = bsdMount("1:2:/", 7),
            .onStep = {},
            .table = [](std::size_t) -> FsResult<std::vector<MountTableRow>> {
                return std::unexpected { core::fs::FsError {
                    .code = FsErrorCode::Unsupported, .systemCode = 0, .context = "scripted" } };
            },
            .mountsAt = {} }) };
    CHECK(isError(context.relation(host / "g", host / "k"), FsErrorCode::Unsupported));
}
