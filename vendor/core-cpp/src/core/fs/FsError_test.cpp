// SPDX-License-Identifier: Apache-2.0
//
// The file-system error vocabulary and the per-OS tables that classify a system code into it. The
// `#ifdef _WIN32` below selects test data, the codes this platform's tables hold; the logic under
// test is the same on every platform.
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <string_view>
#include <type_traits>

#ifdef _WIN32
    #include <windows.h>
#endif

using core::fs::FsError;
using core::fs::FsErrorCode;
using core::fs::fsErrorFromNtStatus;
using core::fs::fsErrorFromSystemCode;
using core::fs::noFollowSymlinkErrnos;
using core::fs::ntStatusTable;
using core::fs::systemErrorTable;

static_assert(std::is_same_v<std::underlying_type_t<FsErrorCode>, std::uint8_t>);
static_assert(FsErrorCode::CrossDevice < FsErrorCode::Last);

namespace
{

constexpr auto CodeCount = static_cast<std::uint8_t>(FsErrorCode::Last);

} // namespace

TEST_CASE("FsErrorCode: every code has a name in lower-case words", "[fs][error]")
{
    for (auto const index: std::views::iota(std::uint8_t { 0 }, CodeCount))
    {
        auto const name = core::fs::toString(static_cast<FsErrorCode>(index));
        CAPTURE(index, name);
        CHECK(name != "unknown error");
        CHECK_FALSE(name.empty());
        CHECK(std::ranges::all_of(
            name, [](char c) { return c == ' ' || (std::islower(static_cast<unsigned char>(c)) != 0); }));
    }
    CHECK(core::fs::toString(FsErrorCode::Last) == "unknown error");
}

TEST_CASE("FsErrorCode: names are distinct", "[fs][error]")
{
    for (auto const left: std::views::iota(std::uint8_t { 0 }, CodeCount))
        for (auto const right: std::views::iota(static_cast<std::uint8_t>(left + 1), CodeCount))
            CHECK(core::fs::toString(static_cast<FsErrorCode>(left))
                  != core::fs::toString(static_cast<FsErrorCode>(right)));
}

TEST_CASE("FsError: toString names the code, the context and the system code", "[fs][error]")
{
    auto const error = FsError { .code = FsErrorCode::NoEntry, .systemCode = 2, .context = "openat" };
    CHECK(error.toString() == "no entry (openat) [system code 2]");
    CHECK(FsError { .code = FsErrorCode::Exist, .systemCode = 0, .context = {} }.toString() == "exist");
}

TEST_CASE("systemErrorTable and ntStatusTable: every row maps below Last", "[fs][error]")
{
    CHECK_FALSE(systemErrorTable().empty());
    for (auto const& row: systemErrorTable())
    {
        CAPTURE(row.systemCode);
        CHECK(row.code < FsErrorCode::Last);
    }
    for (auto const& row: ntStatusTable())
    {
        CAPTURE(row.systemCode);
        CHECK(row.code < FsErrorCode::Last);
    }
}

TEST_CASE("fsErrorFromSystemCode: a missing file is NoEntry and keeps the raw code", "[fs][error]")
{
#ifdef _WIN32
    auto const notFound = std::int64_t { ERROR_FILE_NOT_FOUND };
#else
    auto const notFound = std::int64_t { ENOENT };
#endif
    auto const error = fsErrorFromSystemCode(notFound, "open");
    CHECK(error.code == FsErrorCode::NoEntry);
    CHECK(error.systemCode == notFound);
    CHECK(error.context == "open");
}

TEST_CASE("fsErrorFromSystemCode: a link a call stopped on is Loop", "[fs][error]")
{
#ifdef _WIN32
    auto const stopped = std::int64_t { ERROR_STOPPED_ON_SYMLINK };
#else
    auto const stopped = std::int64_t { ELOOP };
#endif
    CHECK(fsErrorFromSystemCode(stopped, "open").code == FsErrorCode::Loop);
}

TEST_CASE("fsErrorFromSystemCode: an unknown code is Io", "[fs][error]")
{
    auto const error = fsErrorFromSystemCode(987'654'321);
    CHECK(error.code == FsErrorCode::Io);
    CHECK(error.systemCode == 987'654'321);
}

TEST_CASE("fsErrorFromNtStatus: STATUS_OBJECT_NAME_NOT_FOUND where the table has it", "[fs][error]")
{
    // STATUS_OBJECT_NAME_NOT_FOUND, as the signed 32-bit NTSTATUS the API returns.
    auto const notFound = std::int64_t { static_cast<std::int32_t>(0xC0000034U) };
    auto const error = fsErrorFromNtStatus(notFound, "NtCreateFile");
    CHECK(error.systemCode == notFound);
    CHECK(error.context == "NtCreateFile");
#ifdef _WIN32
    CHECK(error.code == FsErrorCode::NoEntry);
#else
    CHECK(ntStatusTable().empty());
    CHECK(error.code == FsErrorCode::Io);
#endif
}

TEST_CASE("fsErrorFromNtStatus: a reparse point met without following is Loop", "[fs][error]")
{
    // STATUS_REPARSE_POINT_ENCOUNTERED: OBJ_DONT_REPARSE met the one component an open names, a
    // link that was not to be followed.
    auto const encountered = std::int64_t { static_cast<std::int32_t>(0xC000050BU) };
    auto const error = fsErrorFromNtStatus(encountered, "NtCreateFile");
#ifdef _WIN32
    CHECK(error.code == FsErrorCode::Loop);
#else
    CHECK(error.code == FsErrorCode::Io);
#endif
}

TEST_CASE("noFollowSymlinkErrnos: ELOOP wherever there is an errno", "[fs][error]")
{
#ifdef _WIN32
    CHECK(noFollowSymlinkErrnos().empty());
#else
    CHECK(std::ranges::find(noFollowSymlinkErrnos(), ELOOP) != noFollowSymlinkErrnos().end());
#endif
}

TEST_CASE("systemErrorTable and ntStatusTable: every code the platform can report has a row", "[fs][error]")
{
    auto const covered = [](FsErrorCode code) {
        auto const holds = [code](core::fs::SystemErrorRow const& row) {
            return row.code == code;
        };
        return std::ranges::any_of(systemErrorTable(), holds) || std::ranges::any_of(ntStatusTable(), holds);
    };
#ifdef _WIN32
    // The codes the Win32 and NT calls of the native descriptors can answer; the POSIX-only ones
    // (WouldBlock, Deadlock, NoTty, TextFileBusy, ...) have no Windows code to come from.
    using enum FsErrorCode;
    for (auto const code: { Access,
                            BadDescriptor,
                            Busy,
                            Quota,
                            Exist,
                            FileTooLarge,
                            IllegalByteSequence,
                            Invalid,
                            IsDirectory,
                            Loop,
                            TooManyLinks,
                            NameTooLong,
                            NoEntry,
                            InsufficientMemory,
                            InsufficientSpace,
                            NotDirectory,
                            NotEmpty,
                            Unsupported,
                            NoSuchDevice,
                            NotPermitted,
                            Pipe,
                            ReadOnly,
                            InvalidSeek,
                            CrossDevice })
    {
        CAPTURE(core::fs::toString(code));
        CHECK(covered(code));
    }
#else
    // Every code is some errno's.
    for (auto const index: std::views::iota(std::uint8_t { 0 }, CodeCount))
    {
        auto const code = static_cast<FsErrorCode>(index);
        CAPTURE(core::fs::toString(code));
        CHECK(covered(code));
    }
#endif
}
