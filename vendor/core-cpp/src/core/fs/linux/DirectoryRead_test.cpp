// SPDX-License-Identifier: Apache-2.0
//
// The records getdents64 writes, built by hand: read one after the other, `.` and `..` skipped, and
// a record that claims more than the bytes returned hold refused as a whole rather than read past.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/DirectoryStream.hpp>
#include <core/fs/linux/DirectoryRead.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>

using core::fs::DescriptorType;
using core::fs::FsErrorCode;
using core::fs::detail::DirectoryBatch;
using core::fs::detail::parseDirentRecords;
using core::fs::test::outcomeOf;

namespace
{

constexpr auto LengthOffset = offsetof(struct ::dirent64, d_reclen);
constexpr auto TypeOffset = offsetof(struct ::dirent64, d_type);
constexpr auto NameOffset = offsetof(struct ::dirent64, d_name);

struct Record
{
    std::string name;
    unsigned char type = DT_REG;
};

/// @return @p records as the kernel writes them: each NUL-terminated name padded to 8 bytes.
std::vector<std::byte> encode(std::vector<Record> const& records)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const& record: records)
    {
        auto const start = bytes.size();
        auto const length = (NameOffset + record.name.size() + 1 + 7) / 8 * 8;
        bytes.resize(start + length);
        auto const reclen = static_cast<std::uint16_t>(length);
        std::memcpy(bytes.data() + start + LengthOffset, &reclen, sizeof(reclen));
        bytes[start + TypeOffset] = std::byte { record.type };
        std::memcpy(bytes.data() + start + NameOffset, record.name.data(), record.name.size());
    }
    return bytes;
}

void setLength(std::vector<std::byte>& bytes, std::size_t record, std::uint16_t length)
{
    std::memcpy(bytes.data() + record + LengthOffset, &length, sizeof(length));
}

} // namespace

TEST_CASE("linux records: each record is read once, without . and ..", "[fs][native][directory]")
{
    auto const bytes =
        encode({ { ".", DT_DIR }, { "..", DT_DIR }, { "file" }, { "dir", DT_DIR }, { "link", DT_LNK } });
    auto batch = DirectoryBatch {};
    REQUIRE(parseDirentRecords(-1, bytes, batch).has_value());
    auto seen = std::vector<std::pair<std::string, DescriptorType>> {};
    for (auto const& entry: batch.entries)
    {
        REQUIRE(entry.has_value());
        seen.emplace_back(entry->name, entry->type);
    }
    CHECK(
        seen
        == std::vector<std::pair<std::string, DescriptorType>> { { "file", DescriptorType::RegularFile },
                                                                 { "dir", DescriptorType::Directory },
                                                                 { "link", DescriptorType::SymbolicLink } });
}

TEST_CASE("linux records: a record beyond what was returned is Invalid", "[fs][native][directory]")
{
    auto const whole = encode({ { "first" }, { "second" } });
    auto const second = static_cast<std::size_t>(whole.size() / 2);
    REQUIRE(second * 2 == whole.size()); // both names pad to one record size
    auto batch = DirectoryBatch {};

    SECTION("a zero length, which would never move on")
    {
        auto bytes = whole;
        setLength(bytes, 0, 0);
        CHECK(outcomeOf(parseDirentRecords(-1, bytes, batch)) == FsErrorCode::Invalid);
    }
    SECTION("a length no longer than the header")
    {
        auto bytes = whole;
        setLength(bytes, 0, static_cast<std::uint16_t>(NameOffset));
        CHECK(outcomeOf(parseDirentRecords(-1, bytes, batch)) == FsErrorCode::Invalid);
    }
    SECTION("a length past the bytes returned")
    {
        auto bytes = whole;
        setLength(bytes, second, static_cast<std::uint16_t>(second + 8));
        CHECK(outcomeOf(parseDirentRecords(-1, bytes, batch)) == FsErrorCode::Invalid);
    }
    SECTION("a name without its NUL")
    {
        auto bytes = whole;
        std::memset(bytes.data() + NameOffset, 'x', second - NameOffset);
        CHECK(outcomeOf(parseDirentRecords(-1, bytes, batch)) == FsErrorCode::Invalid);
    }
    SECTION("a header cut short")
    {
        auto bytes = whole;
        bytes.resize(second + NameOffset - 1);
        CHECK(outcomeOf(parseDirentRecords(-1, bytes, batch)) == FsErrorCode::Invalid);
    }
    SECTION("nothing returned is no record")
    {
        CHECK(parseDirentRecords(-1, {}, batch).has_value());
        CHECK(batch.entries.empty());
    }
    auto intact = DirectoryBatch {};
    CHECK(parseDirentRecords(-1, whole, intact).has_value());
    CHECK(intact.entries.size() == 2);
}
