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
