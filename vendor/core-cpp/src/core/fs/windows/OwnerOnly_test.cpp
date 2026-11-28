// SPDX-License-Identifier: Apache-2.0
//
// What the Windows descriptors owe an owner-only creation: the security descriptor is handed to
// NtCreateFile itself, its owner the process token's user and its DACL protected, with one entry for
// that user, so nothing is inherited and no other account ever holds the object. And what they read
// back: the owner and the DACL of the handle, which every stat open now asks READ_CONTROL for.
//
// Wine is not Windows: it keeps a security descriptor, but its process is neither elevated nor an
// account whose new objects Administrators own, so the elevated case needs native Windows CI.
#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/fs/AccessScope.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/windows/WindowsPathWalk.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <aclapi.h>
#include <sddl.h>
#include <windows.h>

#include "VirtualVolumeTestSupport.hpp"

using core::fs::AccessScope;
using core::fs::CreationAccess;
using core::fs::DescriptorFlags;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;
using core::fs::test::requireElevationOrSkip;
using core::fs::test::VirtualVolume;

namespace
{

/// Frees what a security call allocated with LocalAlloc.
struct LocalFreeDeleter
{
    void operator()(void* memory) const noexcept { ::LocalFree(memory); }
};

/// Closes a handle.
struct HandleCloser
{
    void operator()(void* handle) const noexcept { ::CloseHandle(handle); }
};

using UniqueLocal = std::unique_ptr<void, LocalFreeDeleter>;
using UniqueWinHandle = std::unique_ptr<void, HandleCloser>;

/// @return The process token, for reading.
[[nodiscard]] UniqueWinHandle processToken()
{
    auto token = HANDLE {};
    REQUIRE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE);
    return UniqueWinHandle { token };
}

/// @return The process token's user SID, as bytes.
[[nodiscard]] std::vector<std::byte> tokenUser()
{
    auto const token = processToken();
    auto size = DWORD { 0 };
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    auto buffer = std::vector<std::byte>(size);
    REQUIRE(::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) != FALSE);
    auto const* const user = reinterpret_cast<TOKEN_USER const*>(buffer.data());
    auto sid = std::vector<std::byte>(::GetLengthSid(user->User.Sid));
    REQUIRE(::CopySid(static_cast<DWORD>(sid.size()), sid.data(), user->User.Sid) != FALSE);
    return sid;
}

/// @return Whether the process token is elevated.
[[nodiscard]] bool elevated()
{
    auto const token = processToken();
    auto elevation = TOKEN_ELEVATION {};
    auto size = DWORD { sizeof(elevation) };
    return ::GetTokenInformation(token.get(), TokenElevation, &elevation, size, &size) != FALSE
           && elevation.TokenIsElevated != 0;
}

/// Opens @p path through the host with @p access, as another program would.
[[nodiscard]] UniqueWinHandle hostOpen(std::filesystem::path const& path, DWORD access)
{
    auto const handle = ::CreateFileW(path.c_str(),
                                      access,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    return UniqueWinHandle { handle };
}

/// The owner and DACL of an object, as the host reads them.
struct Security
{
    UniqueLocal descriptor;
    PSID owner = nullptr;
    PACL dacl = nullptr;
};

[[nodiscard]] Security securityOf(std::filesystem::path const& path)
{
    auto const handle = hostOpen(path, READ_CONTROL);
    auto security = Security {};
    auto* descriptor = PSECURITY_DESCRIPTOR {};
    REQUIRE(::GetSecurityInfo(handle.get(),
                              SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &security.owner,
                              nullptr,
                              &security.dacl,
                              nullptr,
                              &descriptor)
            == ERROR_SUCCESS);
    security.descriptor.reset(descriptor);
    return security;
}

/// Adds an entry granting Everyone read and write to the DACL of @p path, through the host.
void grantEveryone(std::filesystem::path const& path, DWORD inheritance = NO_INHERITANCE)
{
    auto const current = securityOf(path);
    auto everyone = EXPLICIT_ACCESS_W {};
    everyone.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    everyone.grfAccessMode = GRANT_ACCESS;
    everyone.grfInheritance = inheritance;
    everyone.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    everyone.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    everyone.Trustee.ptstrName = const_cast<LPWSTR>(L"EVERYONE");
    auto* widened = PACL {};
    REQUIRE(::SetEntriesInAclW(1, &everyone, current.dacl, &widened) == ERROR_SUCCESS);
    auto const owned = UniqueLocal { widened };
    auto const handle = hostOpen(path, READ_CONTROL | WRITE_DAC);
    REQUIRE(::SetSecurityInfo(
                handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, widened, nullptr)
            == ERROR_SUCCESS);
}

/// @return The access @p descriptor's own handle reports.
[[nodiscard]] std::optional<AccessScope> accessOf(DescriptorFixture const& fixture,
                                                  core::fs::IDescriptor& descriptor)
{
    auto const status = fixture.run(descriptor.stat());
    REQUIRE(status.has_value());
    return status->access;
}

[[nodiscard]] std::unique_ptr<core::fs::IDescriptor> createOwnerOnly(DescriptorFixture const& fixture,
                                                                     std::string name)
{
    auto file =
        fixture.run(fixture.root().openAt(SymlinkPolicy::NoFollow,
                                          std::move(name),
                                          OpenFlags::Create | OpenFlags::Exclusive | OpenFlags::OwnerOnly,
                                          DescriptorFlags::Read | DescriptorFlags::Write));
    REQUIRE(file.has_value());
    return std::move(*file);
}

} // namespace

TEST_CASE("windows: stat opens can read the security descriptor", "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    auto& root = fixture.root();
    fixture.createFile("plain", "x");
    REQUIRE(fixture.run(root.createDirectoryAt("dir", CreationAccess::Default)).has_value());
    for (auto const* const name: { "plain", "dir" })
    {
        INFO(name);
        auto const byName = fixture.run(root.statAt(SymlinkPolicy::NoFollow, name));
        REQUIRE(byName.has_value());
        CHECK(byName->access.has_value());
        // A descriptor with no right at all still holds READ_CONTROL for its own stat.
        auto bare =
            fixture.run(root.openAt(SymlinkPolicy::NoFollow, name, OpenFlags::None, DescriptorFlags::None));
        REQUIRE(bare.has_value());
        CHECK(accessOf(fixture, **bare).has_value());
    }
    CHECK(accessOf(fixture, root).has_value());
}

TEST_CASE("windows: an OwnerOnly file is owned by the user, with a protected DACL for the user alone",
          "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    auto const file = createOwnerOnly(fixture, "secret");
    REQUIRE(fixture.run(fixture.root().createDirectoryAt("keys", CreationAccess::OwnerOnly)).has_value());
    auto user = tokenUser();
    for (auto const* const name: { "secret", "keys" })
    {
        INFO(name);
        auto const security = securityOf(fixture.hostDirectory() / name);
        REQUIRE(security.owner != nullptr);
        CHECK(::EqualSid(security.owner, user.data()) != FALSE);
        REQUIRE(security.dacl != nullptr);
        auto control = SECURITY_DESCRIPTOR_CONTROL {};
        auto revision = DWORD {};
        REQUIRE(::GetSecurityDescriptorControl(security.descriptor.get(), &control, &revision) != FALSE);
        CHECK((control & SE_DACL_PROTECTED) != 0);
        auto information = ACL_SIZE_INFORMATION {};
        REQUIRE(::GetAclInformation(security.dacl, &information, sizeof(information), AclSizeInformation)
                != FALSE);
        REQUIRE(information.AceCount == 1);
        auto* entry = LPVOID {};
        REQUIRE(::GetAce(security.dacl, 0, &entry) != FALSE);
        auto const* const allowed = static_cast<ACCESS_ALLOWED_ACE const*>(entry);
        CHECK(allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        CHECK(::EqualSid(const_cast<DWORD*>(&allowed->SidStart), user.data()) != FALSE);
    }
    CHECK(accessOf(fixture, *file) == AccessScope::OwnerOnly);
}

TEST_CASE("windows: an Everyone entry is Shared, read on the handle, not the name", "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    auto& root = fixture.root();
    auto const key = createOwnerOnly(fixture, "key");
    auto const planted = createOwnerOnly(fixture, "planted");
    grantEveryone(fixture.hostDirectory() / "planted");
    CHECK(accessOf(fixture, *planted) == AccessScope::Shared);
    auto const renamed = fixture.run(root.renameAt("planted", &root, "key"));
    if (!renamed && core::fs::test::runsUnderWine())
        SKIP("wine refuses to replace a file that is open ("
             << renamed.error().toString() << "); native Windows replaces it with POSIX semantics");
    REQUIRE(renamed.has_value());
    CHECK(accessOf(fixture, *key) == AccessScope::OwnerOnly);
    CHECK(fixture.run(root.statAt(SymlinkPolicy::NoFollow, "key"))->access == AccessScope::Shared);
}

TEST_CASE("windows: an OwnerOnly file created elevated is owned by the token user", "[fs][native][access]")
{
    // An elevated administrator's new objects are owned by BUILTIN\Administrators by default; the
    // explicit owner keeps the file the user's, so it stays usable unelevated.
    if (core::fs::test::runsUnderWine())
        SKIP("wine runs no elevated process: needs native Windows (the CI runner)");
    if (!elevated())
        requireElevationOrSkip("the process is not elevated");
    auto fixture = DescriptorFixture::createNative();
    auto const file = createOwnerOnly(fixture, "elevated");
    auto user = tokenUser();
    auto const security = securityOf(fixture.hostDirectory() / "elevated");
    REQUIRE(security.owner != nullptr);
    CHECK(::EqualSid(security.owner, user.data()) != FALSE);
    CHECK(accessOf(fixture, *file) == AccessScope::OwnerOnly);
}

TEST_CASE("windows: a foreign owner is ForeignOwner", "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    createOwnerOnly(fixture, "theirs").reset();
    auto administrators = std::vector<std::byte>(SECURITY_MAX_SID_SIZE);
    auto size = static_cast<DWORD>(administrators.size());
    REQUIRE(::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size)
            != FALSE);
    {
        auto const handle = hostOpen(fixture.hostDirectory() / "theirs", READ_CONTROL | WRITE_OWNER);
        if (::SetSecurityInfo(handle.get(),
                              SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION,
                              administrators.data(),
                              nullptr,
                              nullptr,
                              nullptr)
            != ERROR_SUCCESS)
            requireElevationOrSkip(
                "this account may not give a file to BUILTIN\\Administrators (needs elevation)");
    }
    // Opened after the change: wine keeps a security descriptor per open file object, so a handle
    // open before it would still report the old owner there (NTFS keeps one per file).
    auto file = fixture.run(
        fixture.root().openAt(SymlinkPolicy::NoFollow, "theirs", OpenFlags::None, DescriptorFlags::Read));
    REQUIRE(file.has_value());
    CHECK(accessOf(fixture, **file) == AccessScope::ForeignOwner);
    CHECK(fixture.run(fixture.root().statAt(SymlinkPolicy::NoFollow, "theirs"))->access
          == AccessScope::ForeignOwner);
}

TEST_CASE("windows: a NULL DACL is Shared", "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    createOwnerOnly(fixture, "open").reset();
    {
        auto const handle = hostOpen(fixture.hostDirectory() / "open", READ_CONTROL | WRITE_DAC);
        REQUIRE(
            ::SetSecurityInfo(
                handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr)
            == ERROR_SUCCESS);
    }
    auto const security = securityOf(fixture.hostDirectory() / "open");
    if (security.dacl != nullptr)
        SKIP("this file system keeps no NULL DACL (it stored an empty or default one)");
    auto file = fixture.run(
        fixture.root().openAt(SymlinkPolicy::NoFollow, "open", OpenFlags::None, DescriptorFlags::Read));
    REQUIRE(file.has_value());
    CHECK(accessOf(fixture, **file) == AccessScope::Shared);
}

TEST_CASE("windows: an inherit-only entry grants nothing on the directory itself", "[fs][native][access]")
{
    auto fixture = DescriptorFixture::createNative();
    REQUIRE(fixture.run(fixture.root().createDirectoryAt("keys", CreationAccess::OwnerOnly)).has_value());
    // Everyone, for what is created inside only: the directory itself stays the user's.
    grantEveryone(fixture.hostDirectory() / "keys", INHERIT_ONLY | SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    auto directory = fixture.run(
        fixture.root().openAt(SymlinkPolicy::NoFollow, "keys", OpenFlags::Directory, DescriptorFlags::Read));
    REQUIRE(directory.has_value());
    CHECK(accessOf(fixture, **directory) == AccessScope::OwnerOnly);
}

namespace
{

/// The system's NT calls, except that an open of `guarded` asking for READ_CONTROL is refused, as a
/// DACL that withholds it refuses one.
class ReadControlWithheld final: public core::fs::detail::INtFiles
{
  public:
    core::fs::FsResult<core::fs::detail::UniqueHandle> open(
        HANDLE directory,
        std::wstring const& name,
        core::fs::detail::NtOpenRequest const& request) const override
    {
        if (name == L"guarded" && (request.access & READ_CONTROL) != 0)
            return std::unexpected { core::fs::FsError {
                .code = core::fs::FsErrorCode::Access, .systemCode = 0, .context = "NtCreateFile" } };
        return core::fs::detail::systemNtFiles().open(directory, name, request);
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

TEST_CASE("windows: a DACL that withholds READ_CONTROL still stats, with access unknown",
          "[fs][native][access]")
{
    auto home = core::async::testing::ManualExecutor {};
    auto blocking = core::async::testing::ManualExecutor {};
    auto const clock = core::platform::ManualWallClock {};
    auto random = core::fs::test::FixedRandom { 1 };
    auto const directory = core::testing::ScopedTempDir { "core-cpp-fs-read-control" };
    std::filesystem::create_directory(directory.path() / "root");
    std::ofstream { directory.path() / "root" / "guarded" } << "x";
    auto const files = ReadControlWithheld {};
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
        auto const byName = drive(home, blocking, (*root)->statAt(SymlinkPolicy::NoFollow, "guarded"));
        REQUIRE(byName.has_value());
        CHECK(byName->size == 1);
        CHECK_FALSE(byName->access.has_value());
        auto bare = drive(
            home,
            blocking,
            (*root)->openAt(SymlinkPolicy::NoFollow, "guarded", OpenFlags::None, DescriptorFlags::None));
        REQUIRE(bare.has_value());
        auto const status = drive(home, blocking, (*bare)->stat());
        REQUIRE(status.has_value());
        CHECK_FALSE(status->access.has_value());
    }
    fileSystem->reset();
    std::ignore = home.drain() + blocking.drain();
}

TEST_CASE("windows: owner-only creation on FAT is refused and leaves nothing behind",
          "[fs][native][access][windows-native]")
{
    // FAT keeps no security descriptor: what NtCreateFile was given is dropped without an error, so
    // only reading the new object's access back can tell. It is then deleted through its own handle.
    auto const scratch = core::testing::ScopedTempDir { "core-cpp-fs-fat-owner" };
    auto volume = VirtualVolume { scratch.path(), "fat32" };
    auto const root = volume.attach();
    if (!root)
        requireElevationOrSkip("no FAT volume could be attached here: diskpart needs an administrator");
    std::filesystem::create_directories(*root / "grant");
    auto fixture = DescriptorFixture::createNative();
    auto directory = fixture.run(fixture.fileSystem().openPreopen(
        *root / "grant", DescriptorFlags::Read | DescriptorFlags::MutateDirectory));
    INFO("open: " << (directory ? std::string { "ok" } : directory.error().toString()));
    REQUIRE(directory.has_value());

    auto const file =
        fixture.run((*directory)
                        ->openAt(SymlinkPolicy::NoFollow,
                                 "secret",
                                 OpenFlags::Create | OpenFlags::Exclusive | OpenFlags::OwnerOnly,
                                 DescriptorFlags::Read | DescriptorFlags::Write));
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().code == core::fs::FsErrorCode::NotPermitted);
    CHECK_FALSE(std::filesystem::exists(*root / "grant" / "secret"));

    auto const made = fixture.run((*directory)->createDirectoryAt("keys", CreationAccess::OwnerOnly));
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().code == core::fs::FsErrorCode::NotPermitted);
    CHECK_FALSE(std::filesystem::exists(*root / "grant" / "keys"));
}
