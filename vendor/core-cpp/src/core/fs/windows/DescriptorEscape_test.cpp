// SPDX-License-Identifier: Apache-2.0
//
// What only Windows has to get right about a path beneath its descriptor: junctions and mount
// points are links whose target is absolute, so none is traversed, even one that points inside; a
// symbolic link's relative target may be separated by `\`; a link is made a directory link where its
// target is a directory, which other programs need to follow it; and a reparse point that names
// nothing else opens as the file it is. The junctions are made through the host, as an attacker
// outside the guest would make them; making one needs no privilege.
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/windows/NtFile.hpp>
#include <core/fs/windows/ReparsePoint.hpp>
#include <core/fs/windows/WindowsPathWalk.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <winioctl.h>

using core::fs::DescriptorFlags;
using core::fs::DescriptorType;
using core::fs::FsErrorCode;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;
using core::fs::test::FixedRandom;
using core::fs::test::outcomeOf;
using core::fs::test::SymlinkSupport;
using core::fs::test::textOf;

namespace
{

/// Whether the host could make a reparse point.
enum class Made : std::uint8_t
{
    No,
    Yes,
};

/// Writes @p data as the reparse point of the existing @p path, through the host.
Made setReparsePoint(std::filesystem::path const& path, std::span<std::byte const> data)
{
    auto const handle = core::fs::detail::UniqueHandle { ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr) };
    if (handle.get() == INVALID_HANDLE_VALUE)
        return Made::No;
    return core::fs::detail::writeReparseData(handle.get(), data) ? Made::Yes : Made::No;
}

/// Makes @p path a junction to the absolute @p target, as `mklink /J` does.
Made makeJunction(std::filesystem::path const& path, std::wstring const& target)
{
    std::filesystem::create_directory(path);
    return setReparsePoint(path, core::fs::detail::mountPointReparseData(L"\\??\\" + target + L"\\", target));
}

/// @return The `\??\Volume{...}\` name of the volume @p path is on, where the host tells it.
std::optional<std::wstring> volumeNameOf(std::filesystem::path const& path)
{
    auto mountPoint = std::array<wchar_t, MAX_PATH + 1> {};
    auto volume = std::array<wchar_t, MAX_PATH + 1> {};
    if (::GetVolumePathNameW(path.c_str(), mountPoint.data(), static_cast<DWORD>(mountPoint.size())) == FALSE
        || ::GetVolumeNameForVolumeMountPointW(
               mountPoint.data(), volume.data(), static_cast<DWORD>(volume.size()))
               == FALSE)
        return std::nullopt;
    auto name = std::wstring { volume.data() }; // `\\?\Volume{...}\`
    if (!name.starts_with(L"\\\\?\\"))
        return std::nullopt;
    name[1] = L'?';
    name[2] = L'?';
    return name;
}

/// @return What @p name beneath @p fixture's root is, unfollowed where @p policy says so; nothing
///         where the stat was refused.
std::optional<DescriptorType> typeAt(DescriptorFixture const& fixture,
                                     SymlinkPolicy policy,
                                     std::string const& name)
{
    auto const status = fixture.run(fixture.root().statAt(policy, name));
    if (!status)
        return std::nullopt;
    return status->type;
}

/// @return The size of @p name beneath @p fixture's root, following links; nothing where refused.
std::optional<std::uint64_t> sizeAt(DescriptorFixture const& fixture, std::string const& name)
{
    auto const status = fixture.run(fixture.root().statAt(SymlinkPolicy::Follow, name));
    if (!status)
        return std::nullopt;
    return status->size;
}

/// What the host sees a link as, beside a link.
enum class HostEntry : std::uint8_t
{
    File,
    Directory,
};

/// @return What the host sees @p path, unfollowed, as.
HostEntry hostEntryOf(std::filesystem::path const& path)
{
    auto const attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
               ? HostEntry::Directory
               : HostEntry::File;
}

} // namespace

TEST_CASE("windows: a junction or a mount point beneath the preopen is never traversed",
          "[fs][escape][windows]")
{
    auto fixture = DescriptorFixture::createNative();
    auto& root = fixture.root();
    REQUIRE(fixture.run(root.createDirectoryAt("inside", core::fs::CreationAccess::Default)).has_value());
    fixture.createFile("inside/decoy", "inside");
    fixture.writeOutsideFile("decoy", "outside");

    auto names = std::vector<std::string> {};
    if (makeJunction(fixture.hostDirectory() / "out", fixture.outsideDirectory().wstring()) == Made::Yes)
        names.emplace_back("out");
    // Even a junction that points inside is refused: its target is an absolute path, which no walk
    // beneath a handle may take.
    if (makeJunction(fixture.hostDirectory() / "in", (fixture.hostDirectory() / "inside").wstring())
        == Made::Yes)
        names.emplace_back("in");
    if (auto const volume = volumeNameOf(fixture.hostDirectory()))
    {
        std::filesystem::create_directory(fixture.hostDirectory() / "vol");
        if (setReparsePoint(fixture.hostDirectory() / "vol",
                            core::fs::detail::mountPointReparseData(*volume, L""))
            == Made::Yes)
            names.emplace_back("vol");
    }
    if (names.empty())
        SKIP("this file system makes no junction");

    auto const notPermitted = std::optional { FsErrorCode::NotPermitted };
    for (auto const& name: names)
    {
        CAPTURE(name);
        for (auto const policy: { SymlinkPolicy::NoFollow, SymlinkPolicy::Follow })
        {
            CHECK(outcomeOf(fixture.run(root.statAt(policy, name + "/decoy"))) == notPermitted);
            CHECK(outcomeOf(fixture.run(
                      root.openAt(policy, name + "/decoy", OpenFlags::None, DescriptorFlags::Read)))
                  == notPermitted);
            CHECK(outcomeOf(fixture.run(root.metadataHashAt(policy, name + "/."))) == notPermitted);
        }
        CHECK(outcomeOf(fixture.run(root.statAt(SymlinkPolicy::Follow, name))) == notPermitted);
        CHECK(outcomeOf(fixture.run(root.createDirectoryAt(name + "/new", core::fs::CreationAccess::Default)))
              == notPermitted);
        CHECK(outcomeOf(fixture.run(root.unlinkFileAt(name + "/decoy"))) == notPermitted);
        CHECK(outcomeOf(fixture.run(root.readlinkAt(name))) == notPermitted);
        // Unfollowed, it is a link like any other, inside the root.
        CHECK(typeAt(fixture, SymlinkPolicy::NoFollow, name) == DescriptorType::SymbolicLink);
    }
    CHECK(fixture.outsideUnchanged());
    CHECK(sizeAt(fixture, "inside/decoy") == 6U);
}

TEST_CASE("windows: a relative link target separated by a backslash resolves beneath",
          "[fs][escape][windows]")
{
    auto fixture = DescriptorFixture::createNative();
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this account cannot create symbolic links");
    auto& root = fixture.root();
    fixture.writeFile("a/b/f", "through the link");
    REQUIRE(fixture.run(root.symlinkAt("a\\b\\f", "filelink")).has_value());
    REQUIRE(fixture.run(root.symlinkAt("a\\b", "dirlink")).has_value());
    REQUIRE(fixture.run(root.symlinkAt("..\\..\\outside", "a/escape")).has_value());

    auto const file =
        fixture.run(root.openAt(SymlinkPolicy::Follow, "filelink", OpenFlags::None, DescriptorFlags::Read));
    REQUIRE(file.has_value());
    CHECK(textOf(fixture.run((*file)->read(64, 0)).value().bytes) == "through the link");
    CHECK(sizeAt(fixture, "dirlink/f") == 16U);
    CHECK(fixture.run(root.readlinkAt("filelink")).value() == "a\\b\\f");
    CHECK(outcomeOf(fixture.run(root.statAt(SymlinkPolicy::Follow, "a/escape")))
          == FsErrorCode::NotPermitted);

    // What the system itself follows: the host reads the same file through the link.
    CHECK(std::filesystem::file_size(fixture.hostDirectory() / "filelink") == 16);
    CHECK(std::filesystem::is_regular_file(fixture.hostDirectory() / "dirlink" / "f"));
}

TEST_CASE("windows: symlinkAt makes a directory link where the target is a directory", "[fs][windows]")
{
    auto fixture = DescriptorFixture::createNative();
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this account cannot create symbolic links");
    auto& root = fixture.root();
    fixture.writeFile("dir/f", "content");

    struct Row
    {
        char const* target;
        char const* name;
        HostEntry entry;
    };
    for (auto const& row:
         { Row { .target = "dir", .name = "todir", .entry = HostEntry::Directory },
           Row { .target = "dir/f", .name = "tofile", .entry = HostEntry::File },
           Row { .target = "missing", .name = "dangling", .entry = HostEntry::File },
           Row { .target = "missing/", .name = "slashed", .entry = HostEntry::Directory },
           Row { .target = "..", .name = "up", .entry = HostEntry::Directory },
           Row { .target = "dir/../dir/.", .name = "dotted", .entry = HostEntry::Directory } })
    {
        CAPTURE(row.target);
        REQUIRE(fixture.run(root.symlinkAt(row.target, row.name)).has_value());
        CHECK(hostEntryOf(fixture.hostDirectory() / row.name) == row.entry);
        CHECK(typeAt(fixture, SymlinkPolicy::NoFollow, row.name) == DescriptorType::SymbolicLink);
        CHECK(fixture.run(root.readlinkAt(row.name)).value() == row.target);
        REQUIRE(fixture.run(root.unlinkFileAt(row.name)).has_value());
    }
}

TEST_CASE("windows: a reparse point that names nothing else opens as the file it is", "[fs][windows]")
{
    auto fixture = DescriptorFixture::createNative();
    fixture.createFile("tagged", "content");
    // A third-party tag that is no name surrogate, as a cloud or backup filter's own: an 8-byte
    // head, the owner's GUID, then its data. No filter takes it, so the point is the file.
    constexpr auto PrivateTag = std::uint32_t { 0x0000'7F10U };
    auto data = std::vector<std::byte>(8 + 16 + 4);
    auto const length = std::uint16_t { 4 };
    std::memcpy(data.data(), &PrivateTag, sizeof(PrivateTag));
    std::memcpy(data.data() + 4, &length, sizeof(length));
    data[8] = std::byte { 0x5A };
    if (setReparsePoint(fixture.hostDirectory() / "tagged", data) == Made::No)
        SKIP("this file system takes no private reparse tag from this account");

    auto const file = fixture.run(
        fixture.root().openAt(SymlinkPolicy::NoFollow, "tagged", OpenFlags::None, DescriptorFlags::Read));
    REQUIRE(file.has_value());
    CHECK(textOf(fixture.run((*file)->read(64, 0)).value().bytes) == "content");
    CHECK(typeAt(fixture, SymlinkPolicy::NoFollow, "tagged") == DescriptorType::RegularFile);
    CHECK(outcomeOf(fixture.run(fixture.root().readlinkAt("tagged"))) == FsErrorCode::Invalid);
}

TEST_CASE("windows: a link whose target the kernel reads as absolute is never followed",
          "[fs][escape][windows]")
{
    auto fixture = DescriptorFixture::createNative();
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this account cannot create symbolic links");
    auto& root = fixture.root();
    fixture.writeOutsideFile("decoy", "outside");

    auto names = std::vector<std::string> {};
    auto error = std::error_code {};
    std::filesystem::create_symlink(
        fixture.outsideDirectory() / "decoy", fixture.hostDirectory() / "absolute", error);
    if (!error)
        names.emplace_back("absolute");
    // Flagged relative, but read by the kernel from the volume's root, a drive or the object
    // namespace: what a hostile writer of reparse data would plant.
    struct Planted
    {
        char const* name;
        wchar_t const* substitute;
    };
    for (auto const& planted: { Planted { .name = "rooted", .substitute = L"\\x" },
                                Planted { .name = "drive", .substitute = L"C:x" },
                                Planted { .name = "object", .substitute = L"\\??\\C:\\x" } })
    {
        auto const path = fixture.hostDirectory() / planted.name;
        std::ofstream { path }.close();
        if (setReparsePoint(
                path, core::fs::detail::relativeSymlinkReparseData(planted.substitute, planted.substitute))
            == Made::Yes)
            names.emplace_back(planted.name);
    }
    if (names.empty())
        SKIP("this file system takes none of the links");

    for (auto const& name: names)
    {
        CAPTURE(name);
        CHECK(outcomeOf(fixture.run(root.statAt(SymlinkPolicy::Follow, name))) == FsErrorCode::NotPermitted);
        CHECK(outcomeOf(fixture.run(
                  root.openAt(SymlinkPolicy::Follow, name, OpenFlags::None, DescriptorFlags::Read)))
              == FsErrorCode::NotPermitted);
        CHECK(outcomeOf(fixture.run(root.statAt(SymlinkPolicy::NoFollow, name + "/x")))
              == FsErrorCode::NotPermitted);
        CHECK(typeAt(fixture, SymlinkPolicy::NoFollow, name) == DescriptorType::SymbolicLink);
    }
    CHECK(fixture.outsideUnchanged());
}

TEST_CASE("windows: symlinkAt writes no target the kernel would read as absolute, whatever the policy",
          "[fs][windows]")
{
    // Under the POSIX policy `\` and `:` are a name's characters, so the policy lets these through.
    auto fixture = DescriptorFixture::createNative({ .policy = &core::fs::posixPathPolicy() });
    auto& root = fixture.root();
    CHECK(outcomeOf(fixture.run(root.symlinkAt("\\x", "rooted"))) == FsErrorCode::NotPermitted);
    CHECK(outcomeOf(fixture.run(root.symlinkAt("\\??\\C:\\x", "object"))) == FsErrorCode::NotPermitted);
    CHECK(outcomeOf(fixture.run(root.symlinkAt("C:x", "drive"))) == FsErrorCode::NotPermitted);
    CHECK(outcomeOf(fixture.run(root.symlinkAt("a/b:stream", "stream"))) == FsErrorCode::Invalid);
    for (auto const* name: { "rooted", "object", "drive", "stream" })
        CHECK(outcomeOf(fixture.run(root.statAt(SymlinkPolicy::NoFollow, name))) == FsErrorCode::NoEntry);
}

namespace
{

/// The system's NT calls, but for one name: it reads as a reparse point that names nothing else,
/// and opening it again through its filter meets a mount point, as when a writer swaps the one
/// for the other between the check and the reopen.
class SwappedPointFiles final: public core::fs::detail::INtFiles
{
  public:
    core::fs::FsResult<core::fs::detail::UniqueHandle> open(
        HANDLE directory,
        std::wstring const& name,
        core::fs::detail::NtOpenRequest const& request) const override
    {
        if (request.reparse == core::fs::detail::ReparseHandling::ThroughFilters && isSwapped(directory))
            return std::unexpected { core::fs::fsErrorFromNtStatus(
                std::int64_t { STATUS_REPARSE_POINT_ENCOUNTERED }, "scripted") };
        auto opened = core::fs::detail::systemNtFiles().open(directory, name, request);
        if (opened && name == L"swapped")
            _swapped = core::fs::detail::systemNtFiles().identity(opened->get()).value();
        return opened;
    }

    core::fs::FsResult<core::fs::detail::EntryAttributes> attributes(HANDLE handle) const override
    {
        if (isSwapped(handle))
            return core::fs::detail::EntryAttributes { .attributes = FILE_ATTRIBUTE_REPARSE_POINT,
                                                       .reparseTag = 0x0000'7F10U };
        return core::fs::detail::systemNtFiles().attributes(handle);
    }

    core::fs::FsResult<core::fs::detail::FileIdentity> identity(HANDLE handle) const override
    {
        return core::fs::detail::systemNtFiles().identity(handle);
    }

    core::fs::FsResult<void> queryDirectory(HANDLE directory, std::span<std::byte> records) const override
    {
        return core::fs::detail::systemNtFiles().queryDirectory(directory, records);
    }

  private:
    /// By identity, not by handle value: a closed handle's value is reused.
    [[nodiscard]] bool isSwapped(HANDLE handle) const
    {
        auto const identity = core::fs::detail::systemNtFiles().identity(handle);
        return _swapped && identity && *identity == *_swapped;
    }

    mutable std::optional<core::fs::detail::FileIdentity> _swapped;
};

/// Drives @p task to its end over the two executors, bounded.
template <typename T>
T drive(core::async::testing::ManualExecutor& home,
        core::async::testing::ManualExecutor& blocking,
        core::async::Task<T> task)
{
    task.handle().resume();
    for ([[maybe_unused]] auto const round: std::views::iota(0, 10'000))
        if (task.done() || home.drain() + blocking.drain() == 0)
            break;
    REQUIRE(task.done());
    return task.result();
}

} // namespace

TEST_CASE("windows: a point swapped for a mount point before its filter opens it is refused",
          "[fs][escape][windows]")
{
    auto home = core::async::testing::ManualExecutor {};
    auto blocking = core::async::testing::ManualExecutor {};
    auto const clock = core::platform::ManualWallClock {};
    auto random = FixedRandom { 1 };
    auto const files = SwappedPointFiles {};
    auto const directory = core::testing::ScopedTempDir { "core-cpp-fs-swapped" };
    std::ofstream { directory.path() / "swapped" } << "stub";
    std::ofstream { directory.path() / "plain" } << "plain";
    auto fileSystem =
        core::fs::detail::makeWindowsDescriptorFileSystem(home, blocking, clock, random, {}, files);
    REQUIRE(fileSystem.has_value());
    {
        auto root = drive(
            home,
            blocking,
            (*fileSystem)
                ->openPreopen(directory.path(), DescriptorFlags::Read | DescriptorFlags::MutateDirectory));
        REQUIRE(root.has_value());
        auto const swapped = drive(
            home,
            blocking,
            (*root)->openAt(SymlinkPolicy::NoFollow, "swapped", OpenFlags::None, DescriptorFlags::Read));
        CHECK(outcomeOf(swapped) == FsErrorCode::NotPermitted);
        auto const plain =
            drive(home,
                  blocking,
                  (*root)->openAt(SymlinkPolicy::NoFollow, "plain", OpenFlags::None, DescriptorFlags::Read));
        CHECK(plain.has_value());
    }
    fileSystem->reset();
    std::ignore = home.drain() + blocking.drain();
}

namespace
{

/// The system's NT calls, but for one file: its append handle, opened again by its handle, lands on
/// another file outside, as following a symbolic link put on the file after its check would.
class RedirectedAppendFiles final: public core::fs::detail::INtFiles
{
  public:
    explicit RedirectedAppendFiles(std::filesystem::path outside): _outside { std::move(outside) } {}

    core::fs::FsResult<core::fs::detail::UniqueHandle> open(
        HANDLE directory,
        std::wstring const& name,
        core::fs::detail::NtOpenRequest const& request) const override
    {
        auto const& system = core::fs::detail::systemNtFiles();
        if (name.empty() && (request.access & FILE_APPEND_DATA) != 0 && _victim
            && system.identity(directory).value() == *_victim)
            return core::fs::detail::UniqueHandle { ::CreateFileW(_outside.c_str(),
                                                                  FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                                                                  FILE_SHARE_READ | FILE_SHARE_WRITE
                                                                      | FILE_SHARE_DELETE,
                                                                  nullptr,
                                                                  OPEN_EXISTING,
                                                                  0,
                                                                  nullptr) };
        auto opened = system.open(directory, name, request);
        if (opened && name == L"victim")
            _victim = system.identity(opened->get()).value();
        return opened;
    }

    core::fs::FsResult<core::fs::detail::EntryAttributes> attributes(HANDLE handle) const override
    {
        return core::fs::detail::systemNtFiles().attributes(handle);
    }

    core::fs::FsResult<core::fs::detail::FileIdentity> identity(HANDLE handle) const override
    {
        return core::fs::detail::systemNtFiles().identity(handle);
    }

    core::fs::FsResult<void> queryDirectory(HANDLE directory, std::span<std::byte> records) const override
    {
        return core::fs::detail::systemNtFiles().queryDirectory(directory, records);
    }

  private:
    std::filesystem::path _outside;
    mutable std::optional<core::fs::detail::FileIdentity> _victim;
};

} // namespace

TEST_CASE("windows: an append handle that opens another object than the file is refused",
          "[fs][escape][windows]")
{
    auto home = core::async::testing::ManualExecutor {};
    auto blocking = core::async::testing::ManualExecutor {};
    auto const clock = core::platform::ManualWallClock {};
    auto random = FixedRandom { 1 };
    auto const directory = core::testing::ScopedTempDir { "core-cpp-fs-append" };
    std::filesystem::create_directory(directory.path() / "root");
    std::ofstream { directory.path() / "root" / "victim" } << "inside";
    std::ofstream { directory.path() / "outside" } << "outside";
    auto const files = RedirectedAppendFiles { directory.path() / "outside" };
    auto fileSystem =
        core::fs::detail::makeWindowsDescriptorFileSystem(home, blocking, clock, random, {}, files);
    REQUIRE(fileSystem.has_value());
    {
        auto root = drive(home,
                          blocking,
                          (*fileSystem)
                              ->openPreopen(directory.path() / "root",
                                            DescriptorFlags::Read | DescriptorFlags::MutateDirectory));
        REQUIRE(root.has_value());
        auto const victim = drive(
            home,
            blocking,
            (*root)->openAt(SymlinkPolicy::NoFollow, "victim", OpenFlags::None, DescriptorFlags::Write));
        CHECK(outcomeOf(victim) == FsErrorCode::NotPermitted);
    }
    fileSystem->reset();
    std::ignore = home.drain() + blocking.drain();
    CHECK(std::filesystem::file_size(directory.path() / "outside") == 7);
}
