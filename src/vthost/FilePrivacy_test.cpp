// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <vthost/FilePrivacy.hpp>

#ifdef _WIN32
// clang-format off
    #include <winsock2.h>
    #include <windows.h>
    #include <aclapi.h>
// clang-format on
#endif

using namespace vthost;
namespace fs = std::filesystem;

TEST_CASE("NativeFilePrivacy leaves a file to its owner alone", "[vthost][snapshot][privacy]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-privacy" };
    auto const file = dir.path() / "snapshot";
    auto const folder = dir.path() / "folder";
    std::ofstream { file } << "x";
    fs::create_directory(folder);

    REQUIRE(NativeFilePrivacy::instance().restrictToOwner(file, PrivateEntry::File));
    REQUIRE(NativeFilePrivacy::instance().restrictToOwner(folder, PrivateEntry::Directory));

#ifndef _WIN32
    CHECK(fs::status(file).permissions() == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK(fs::status(folder).permissions() == fs::perms::owner_all);
#else
    auto dacl = PACL { nullptr };
    auto descriptor = PSECURITY_DESCRIPTOR { nullptr };
    REQUIRE(GetNamedSecurityInfoW(file.c_str(),
                                  SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION,
                                  nullptr,
                                  nullptr,
                                  &dacl,
                                  nullptr,
                                  &descriptor)
            == ERROR_SUCCESS);
    auto control = SECURITY_DESCRIPTOR_CONTROL {};
    auto revision = DWORD {};
    REQUIRE(GetSecurityDescriptorControl(descriptor, &control, &revision));
    CHECK((control & SE_DACL_PROTECTED) != 0);
    CHECK(dacl->AceCount == 1);
    LocalFree(descriptor);
#endif
}

TEST_CASE("NativeFilePrivacy reports a path it cannot restrict", "[vthost][snapshot][privacy]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-privacy" };
    auto const result =
        NativeFilePrivacy::instance().restrictToOwner(dir.path() / "missing", PrivateEntry::File);
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("a private file must be the user's own and closed to everyone else", "[vthost][privacy]")
{
    auto const path = fs::path { "/state/tls/default.pem" };
    CHECK(checkPrivateFile(FileOwnership { .ownerId = 1000, .mode = 0600 }, 1000, path));
    CHECK(checkPrivateFile(FileOwnership { .ownerId = 1000, .mode = 0400 }, 1000, path));

    // Another user's file: planted by them, whatever its mode.
    auto const foreign = checkPrivateFile(FileOwnership { .ownerId = 1001, .mode = 0600 }, 1000, path);
    REQUIRE_FALSE(foreign);
    CHECK(foreign.error().contains("belongs to user 1001"));
    CHECK(foreign.error().contains(path.string()));

    // Ours, but open to the group or to others, for reading or for writing.
    for (auto const mode: { 0640U, 0604U, 0620U, 0602U, 0644U, 0660U, 0666U })
    {
        auto const lax = checkPrivateFile(FileOwnership { .ownerId = 1000, .mode = mode }, 1000, path);
        REQUIRE_FALSE(lax);
        CHECK(lax.error().contains("chmod 600"));
    }
}

#ifndef _WIN32
TEST_CASE("NativeFilePrivacy reads a private file and refuses a lax one", "[vthost][privacy]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-privacy" };
    auto const file = dir.path() / "identity.pem";
    std::ofstream { file } << "secret";

    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    CHECK(NativeFilePrivacy::instance().readPrivateFile(file) == std::string { "secret" });

    fs::permissions(file,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read
                        | fs::perms::others_read,
                    fs::perm_options::replace);
    auto const lax = NativeFilePrivacy::instance().readPrivateFile(file);
    REQUIRE_FALSE(lax);
    CHECK(lax.error().contains("mode 0644"));

    // Neither a link to it, nor something that is not a file.
    auto const link = dir.path() / "link.pem";
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    fs::create_symlink(file, link);
    CHECK_FALSE(NativeFilePrivacy::instance().readPrivateFile(link));
    CHECK_FALSE(NativeFilePrivacy::instance().readPrivateFile(dir.path()));
    CHECK_FALSE(NativeFilePrivacy::instance().readPrivateFile(dir.path() / "missing"));
}
#endif
