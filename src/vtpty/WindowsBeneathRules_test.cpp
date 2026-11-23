// SPDX-License-Identifier: Apache-2.0
//
// What a Windows entry is to a read beneath a directory, as a pure rule: its kind from attributes, reparse
// tag and handle type, and a FILETIME as Unix milliseconds. Pure, so tested on every platform.

#include <vtpty/WindowsBeneathRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
constexpr auto ArchiveAttribute = uint32_t { 0x20 };
}

TEST_CASE("windowsFileKindOf: plain files and directories", "[vtpty][windowsbeneathrules]")
{
    using vtpty::FileKind;
    using vtpty::WindowsHandleType;
    namespace A = vtpty::WindowsAttribute;
    CHECK(vtpty::windowsFileKindOf(
              { .attributes = ArchiveAttribute, .reparseTag = 0, .handleType = WindowsHandleType::Disk })
          == FileKind::Regular);
    CHECK(vtpty::windowsFileKindOf(
              { .attributes = A::Directory, .reparseTag = 0, .handleType = WindowsHandleType::Disk })
          == FileKind::Directory);
    CHECK(
        vtpty::windowsFileKindOf({ .attributes = 0, .reparseTag = 0, .handleType = WindowsHandleType::Other })
        == FileKind::Other);
}

TEST_CASE("windowsFileKindOf: a name surrogate is a link, file or directory", "[vtpty][windowsbeneathrules]")
{
    using vtpty::FileKind;
    using vtpty::WindowsHandleType;
    namespace A = vtpty::WindowsAttribute;
    for (auto const tag: { uint32_t { 0xA000'000C }, uint32_t { 0xA000'0003 } })
    {
        CHECK(vtpty::windowsFileKindOf(
                  { .attributes = A::ReparsePoint, .reparseTag = tag, .handleType = WindowsHandleType::Disk })
              == FileKind::Symlink);
        CHECK(vtpty::windowsFileKindOf({ .attributes = A::ReparsePoint | A::Directory,
                                         .reparseTag = tag,
                                         .handleType = WindowsHandleType::Disk })
              == FileKind::Symlink);
    }
}

TEST_CASE("windowsFileKindOf: storage tags are what they store", "[vtpty][windowsbeneathrules]")
{
    using vtpty::FileKind;
    using vtpty::WindowsHandleType;
    namespace A = vtpty::WindowsAttribute;
    for (auto const tag: { uint32_t { 0x8000'0013 },
                           uint32_t { 0x8000'0017 },
                           uint32_t { 0x9000'001A },
                           uint32_t { 0x9000'301A },
                           uint32_t { 0x9000'F01A } })
    {
        CHECK(vtpty::windowsFileKindOf(
                  { .attributes = A::ReparsePoint, .reparseTag = tag, .handleType = WindowsHandleType::Disk })
              == FileKind::Regular);
        CHECK(vtpty::windowsFileKindOf({ .attributes = A::ReparsePoint | A::Directory,
                                         .reparseTag = tag,
                                         .handleType = WindowsHandleType::Disk })
              == FileKind::Directory);
    }
}

TEST_CASE("windowsFileKindOf: any other reparse tag is something else", "[vtpty][windowsbeneathrules]")
{
    using vtpty::FileKind;
    using vtpty::WindowsHandleType;
    namespace A = vtpty::WindowsAttribute;
    for (auto const tag: { uint32_t { 0x8000'0023 },
                           uint32_t { 0x8000'001B },
                           uint32_t { 0x1234'5678 },
                           uint32_t { 0x9000'001B } })
    {
        CHECK(vtpty::windowsFileKindOf(
                  { .attributes = A::ReparsePoint, .reparseTag = tag, .handleType = WindowsHandleType::Disk })
              == FileKind::Other);
        CHECK(vtpty::windowsFileKindOf({ .attributes = A::ReparsePoint | A::Directory,
                                         .reparseTag = tag,
                                         .handleType = WindowsHandleType::Disk })
              == FileKind::Other);
    }
}

TEST_CASE("windowsFileKindOf: a file whose data is not on disk is something else",
          "[vtpty][windowsbeneathrules]")
{
    using vtpty::FileKind;
    using vtpty::WindowsHandleType;
    namespace A = vtpty::WindowsAttribute;
    for (auto const extra: { A::RecallOnDataAccess, A::Offline })
    {
        CHECK(vtpty::windowsFileKindOf({ .attributes = ArchiveAttribute | extra,
                                         .reparseTag = 0,
                                         .handleType = WindowsHandleType::Disk })
              == FileKind::Other);
        CHECK(vtpty::windowsFileKindOf({ .attributes = A::ReparsePoint | extra,
                                         .reparseTag = 0x9000'001A,
                                         .handleType = WindowsHandleType::Disk })
              == FileKind::Other);
        CHECK(vtpty::windowsFileKindOf({ .attributes = A::Directory | extra,
                                         .reparseTag = 0,
                                         .handleType = WindowsHandleType::Disk })
              == FileKind::Directory);
    }
}

TEST_CASE("unixMsOfFileTime", "[vtpty][windowsbeneathrules]")
{
    CHECK(vtpty::unixMsOfFileTime(116'444'736'000'000'000ULL) == 0);
    CHECK(vtpty::unixMsOfFileTime(116'444'736'000'000'000ULL + 12'345'678) == 1234);
    CHECK(vtpty::unixMsOfFileTime(0) == 0);
    CHECK(vtpty::unixMsOfFileTime(133'000'000'000'000'000ULL) == 1'655'526'400'000ULL);
}
