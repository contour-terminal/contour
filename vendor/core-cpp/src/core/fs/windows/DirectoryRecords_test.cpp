// SPDX-License-Identifier: Apache-2.0
//
// The records a Windows directory stream reads, built by hand: what each entry is, `.` and `..`
// skipped, a name UTF-8 cannot spell refused for its entry alone, and a record that would read
// beyond its buffer refused as a whole.
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/windows/DirectoryRecords.hpp>
#include <core/fs/windows/NtFile.hpp>
#include <core/fs/windows/WindowsPathWalk.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using core::fs::DescriptorType;
using core::fs::FsErrorCode;
using core::fs::detail::parseDirectoryRecords;
using core::fs::test::outcomeOf;

namespace
{

/// One record to write: its name in UTF-16, its attributes and, for a reparse point, its tag.
struct Record
{
    std::wstring name;
    std::uint32_t attributes = FILE_ATTRIBUTE_NORMAL;
    std::uint32_t reparseTag = 0;
};

void writeWord(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// @return @p records chained as the system writes them, each 8-byte aligned.
std::vector<std::byte> encode(std::vector<Record> const& records)
{
    auto bytes = std::vector<std::byte> {};
    auto previous = std::size_t { 0 };
    for (auto const& record: records)
    {
        auto const start = bytes.size();
        if (start > 0)
            writeWord(bytes,
                      previous + offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset),
                      static_cast<std::uint32_t>(start - previous));
        auto const nameBytes = record.name.size() * sizeof(wchar_t);
        auto const size = offsetof(FILE_ID_BOTH_DIR_INFO, FileName) + nameBytes;
        bytes.resize(start + ((size + 7) / 8 * 8));
        writeWord(bytes, start + offsetof(FILE_ID_BOTH_DIR_INFO, FileAttributes), record.attributes);
        writeWord(bytes,
                  start + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength),
                  static_cast<std::uint32_t>(nameBytes));
        writeWord(bytes, start + offsetof(FILE_ID_BOTH_DIR_INFO, EaSize), record.reparseTag);
        std::memcpy(
            bytes.data() + start + offsetof(FILE_ID_BOTH_DIR_INFO, FileName), record.name.data(), nameBytes);
        previous = start;
    }
    return bytes;
}

} // namespace

TEST_CASE("windows records: each entry is what its attributes say, without . and ..", "[fs][windows]")
{
    auto const bytes = encode({
        { .name = L".", .attributes = FILE_ATTRIBUTE_DIRECTORY },
        { .name = L"..", .attributes = FILE_ATTRIBUTE_DIRECTORY },
        { .name = L"file.txt" },
        { .name = L"dir", .attributes = FILE_ATTRIBUTE_DIRECTORY },
        { .name = L"link", .attributes = FILE_ATTRIBUTE_REPARSE_POINT, .reparseTag = IO_REPARSE_TAG_SYMLINK },
        { .name = L"junction",
          .attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
          .reparseTag = IO_REPARSE_TAG_MOUNT_POINT },
        // A placeholder names nothing else: it is the directory it is.
        { .name = L"cloud",
          .attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
          .reparseTag = IO_REPARSE_TAG_CLOUD_6 },
        // The tag field is an EA size where the entry is no reparse point.
        { .name = L"ea", .attributes = FILE_ATTRIBUTE_NORMAL, .reparseTag = IO_REPARSE_TAG_SYMLINK },
        { .name = L"café" },
    });
    auto const entries = parseDirectoryRecords(bytes);
    REQUIRE(entries.has_value());
    auto seen = std::vector<std::pair<std::string, DescriptorType>> {};
    for (auto const& entry: *entries)
    {
        REQUIRE(entry.has_value());
        seen.emplace_back(entry->name, entry->type);
    }
    CHECK(seen
          == std::vector<std::pair<std::string, DescriptorType>> {
              { "file.txt", DescriptorType::RegularFile },
              { "dir", DescriptorType::Directory },
              { "link", DescriptorType::SymbolicLink },
              { "junction", DescriptorType::SymbolicLink },
              { "cloud", DescriptorType::Directory },
              { "ea", DescriptorType::RegularFile },
              { "caf\xc3\xa9", DescriptorType::RegularFile },
          });
}

TEST_CASE("windows records: an unpaired surrogate is refused for its entry alone", "[fs][windows]")
{
    auto const bytes =
        encode({ { .name = L"a" }, { .name = std::wstring { L'b', wchar_t { 0xD800 } } }, { .name = L"c" } });
    auto const entries = parseDirectoryRecords(bytes);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 3);
    CHECK((*entries)[0]->name == "a");
    CHECK(outcomeOf((*entries)[1]) == FsErrorCode::IllegalByteSequence);
    CHECK((*entries)[2]->name == "c");
}

TEST_CASE("windows records: a record beyond its buffer is Invalid", "[fs][windows]")
{
    auto const whole = encode({ { .name = L"first" }, { .name = L"second" } });

    SECTION("a name longer than what is left")
    {
        auto bytes = whole;
        bytes.resize(offsetof(FILE_ID_BOTH_DIR_INFO, FileName) + 2);
        writeWord(bytes, offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset), 0);
        CHECK(outcomeOf(parseDirectoryRecords(bytes)) == FsErrorCode::Invalid);
    }
    SECTION("a next record past the end")
    {
        auto bytes = whole;
        writeWord(bytes,
                  offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset),
                  static_cast<std::uint32_t>(bytes.size()));
        CHECK(outcomeOf(parseDirectoryRecords(bytes)) == FsErrorCode::Invalid);
    }
    SECTION("a chain that does not move forward")
    {
        auto bytes = whole;
        writeWord(bytes, offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset), 1);
        CHECK(outcomeOf(parseDirectoryRecords(bytes)) == FsErrorCode::Invalid);
    }
    SECTION("an odd name length")
    {
        auto bytes = whole;
        writeWord(bytes, offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength), 3);
        CHECK(outcomeOf(parseDirectoryRecords(bytes)) == FsErrorCode::Invalid);
    }
    SECTION("a buffer too short for one record")
    {
        auto bytes = whole;
        bytes.resize(8);
        CHECK(outcomeOf(parseDirectoryRecords(bytes)) == FsErrorCode::Invalid);
    }
    CHECK(parseDirectoryRecords(whole).has_value());
}

TEST_CASE("windows records: a name no file system gives is refused for its entry alone", "[fs][windows]")
{
    auto const bytes = encode(
        { { .name = L"a" }, { .name = L"" }, { .name = L"b\\c" }, { .name = L"d/e" }, { .name = L"f" } });
    auto const entries = parseDirectoryRecords(bytes);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 5);
    CHECK((*entries)[0]->name == "a");
    for (auto const index: { 1, 2, 3 })
        CHECK(outcomeOf((*entries)[static_cast<std::size_t>(index)]) == FsErrorCode::Io);
    CHECK((*entries)[4]->name == "f");
}

namespace
{

/// The system's NT calls, but for listing: each call takes the next scripted Win32 code, and a 0
/// lists through the system.
class ScriptedListing final: public core::fs::detail::INtFiles
{
  public:
    explicit ScriptedListing(std::deque<DWORD> codes): _codes { std::move(codes) } {}

    core::fs::FsResult<core::fs::detail::UniqueHandle> open(
        HANDLE directory,
        std::wstring const& name,
        core::fs::detail::NtOpenRequest const& request) const override
    {
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
        REQUIRE_FALSE(_codes.empty());
        auto const code = _codes.front();
        _codes.pop_front();
        if (code == 0)
            return core::fs::detail::systemNtFiles().queryDirectory(directory, records);
        return std::unexpected { core::fs::fsErrorFromSystemCode(std::int64_t { code }, "scripted") };
    }

  private:
    mutable std::deque<DWORD> _codes;
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

/// What a stream over a directory holding `f` answers, call by call, when its listing calls answer
/// @p codes.
std::vector<core::fs::test::Outcome> streamOver(std::deque<DWORD> codes)
{
    auto home = core::async::testing::ManualExecutor {};
    auto blocking = core::async::testing::ManualExecutor {};
    auto const clock = core::platform::ManualWallClock {};
    auto random = core::fs::test::FixedRandom { 1 };
    auto const files = ScriptedListing { std::move(codes) };
    auto const directory = core::testing::ScopedTempDir { "core-cpp-fs-listing" };
    std::ofstream { directory.path() / "f" } << "x";
    auto answers = std::vector<core::fs::test::Outcome> {};
    auto fileSystem =
        core::fs::detail::makeWindowsDescriptorFileSystem(home, blocking, clock, random, {}, files);
    REQUIRE(fileSystem.has_value());
    {
        auto root = drive(
            home, blocking, (*fileSystem)->openPreopen(directory.path(), core::fs::DescriptorFlags::Read));
        REQUIRE(root.has_value());
        auto stream = drive(home, blocking, (*root)->readDirectory());
        REQUIRE(stream.has_value());
        for ([[maybe_unused]] auto const call: std::views::iota(0, 8))
        {
            auto const next = drive(home, blocking, (*stream)->next());
            if (next && !*next)
                break;
            answers.push_back(outcomeOf(next));
        }
    }
    fileSystem->reset();
    std::ignore = home.drain() + blocking.drain();
    return answers;
}

} // namespace

TEST_CASE("windows listing: a directory with no entry at all ends at once", "[fs][windows]")
{
    using Outcomes = std::vector<core::fs::test::Outcome>;
    // An empty FAT or exFAT root: not even `.` and `..`, which the first call reports as no file.
    CHECK(streamOver({ ERROR_FILE_NOT_FOUND }).empty());
    CHECK(streamOver({ ERROR_NO_MORE_FILES }).empty());
    // After a batch, the same code is a failure, answered once, and ends the stream.
    CHECK(streamOver({ 0, ERROR_FILE_NOT_FOUND }) == Outcomes { std::nullopt, FsErrorCode::NoEntry });
    CHECK(streamOver({ 0, ERROR_NO_MORE_FILES }) == Outcomes { std::nullopt });
    CHECK(streamOver({ ERROR_ACCESS_DENIED }) == Outcomes { FsErrorCode::Access });
}
