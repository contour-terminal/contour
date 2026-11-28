// SPDX-License-Identifier: Apache-2.0
//
// The file systems whose file ids relation() trusts. A row is added only for a file system whose id
// is persistent and unique on its volume: NTFS's file reference and ReFS's 128-bit id are; FAT's and
// exFAT's are the position of a directory entry, which a rename moves, and an SMB share's are the
// server's, persistent only where the server says so -- which the name alone does not tell, so a
// share stays unsupported. A name not in the table is refused: fail closed.
#include <core/fs/windows/StableFileIdRows.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/detail/Ancestry.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string_view>

namespace core::fs::detail
{

namespace
{

    using namespace std::string_view_literals;

    constexpr auto Rows = std::to_array<StableFileIdRow>({
        { .fileSystemName = L"NTFS"sv, .stability = FileIdStability::Stable },
        { .fileSystemName = L"ReFS"sv, .stability = FileIdStability::Stable },
    });

    [[nodiscard]] std::uint64_t littleEndian(std::span<std::byte const, 8> bytes) noexcept
    {
        auto value = std::uint64_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
            value |= std::uint64_t { std::to_integer<std::uint8_t>(bytes[index]) } << (8U * index);
        return value;
    }

} // namespace

std::span<StableFileIdRow const> stableFileIdRows() noexcept
{
    return Rows;
}

FileIdStability fileIdStabilityOf(std::wstring_view fileSystemName) noexcept
{
    auto const row = std::ranges::find(Rows, fileSystemName, &StableFileIdRow::fileSystemName);
    return row == Rows.end() ? FileIdStability::Unstable : row->stability;
}

FsResult<ObjectIdentity> stableIdentityOf(std::uint64_t volumeSerial,
                                          std::span<std::byte const, 16> fileId,
                                          std::wstring_view fileSystemName)
{
    auto const identity = ObjectIdentity { .device = volumeSerial,
                                           .idLow = littleEndian(fileId.first<8>()),
                                           .idHigh = littleEndian(fileId.last<8>()) };
    if (fileIdStabilityOf(fileSystemName) != FileIdStability::Stable
        || (identity.idLow == 0 && identity.idHigh == 0))
        return std::unexpected { FsError { .code = FsErrorCode::Unsupported,
                                           .systemCode = 0,
                                           .context = "this file system gives no stable file identities" } };
    return identity;
}

} // namespace core::fs::detail
