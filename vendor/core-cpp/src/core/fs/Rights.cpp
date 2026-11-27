// SPDX-License-Identifier: Apache-2.0
#include <core/fs/Rights.hpp>

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>

namespace core::fs
{

namespace
{

    using enum DescriptorFlags;

    // Read and resolve need Read; change an entry, MutateDirectory; change a file's bytes, Write.
    // SetTimes is the one row with a choice: a file's times need Write, a directory's MutateDirectory.
    // A directory never carries Write and a file never MutateDirectory (grantedFlags), so each kind
    // meets the row only with its own flag.
    constexpr auto RightsRows = std::to_array<RightsRow>({
        { .operation = Operation::Read, .required = Read },
        { .operation = Operation::Write, .required = Write },
        { .operation = Operation::Append, .required = Write },
        { .operation = Operation::Advise, .required = None },
        { .operation = Operation::Sync, .required = None },
        { .operation = Operation::SetSize, .required = Write },
        { .operation = Operation::SetTimes, .required = Write | MutateDirectory },
        { .operation = Operation::Stat, .required = None },
        { .operation = Operation::MetadataHash, .required = None },
        { .operation = Operation::ReadDirectory, .required = Read },
        { .operation = Operation::OpenAt, .required = Read },
        { .operation = Operation::CreateDirectoryAt, .required = MutateDirectory },
        { .operation = Operation::StatAt, .required = Read },
        { .operation = Operation::SetTimesAt, .required = MutateDirectory },
        { .operation = Operation::LinkAt, .required = MutateDirectory },
        { .operation = Operation::ReadlinkAt, .required = Read },
        { .operation = Operation::RemoveDirectoryAt, .required = MutateDirectory },
        { .operation = Operation::RenameAt, .required = MutateDirectory },
        { .operation = Operation::SymlinkAt, .required = MutateDirectory },
        { .operation = Operation::UnlinkFileAt, .required = MutateDirectory },
        { .operation = Operation::MetadataHashAt, .required = Read },
    });
    static_assert(RightsRows.size() == static_cast<std::size_t>(Operation::Last));

    constexpr bool rowsFollowTheEnumeration() noexcept
    {
        return std::ranges::all_of(
            std::views::iota(std::size_t { 0 }, RightsRows.size()), [](std::size_t index) {
                return static_cast<std::size_t>(RightsRows.at(index).operation) == index;
            });
    }
    static_assert(rowsFollowTheEnumeration(), "RightsRows must hold one row per Operation, in its order");

    std::unexpected<FsError> refuse(FsErrorCode code) noexcept
    {
        return std::unexpected { FsError { .code = code, .systemCode = 0, .context = {} } };
    }

} // namespace

std::span<RightsRow const> rightsTable() noexcept
{
    return RightsRows;
}

FsResult<void> checkRights(DescriptorFlags held, Operation operation) noexcept
{
    auto const row = std::ranges::find(RightsRows, operation, &RightsRow::operation);
    if (row != RightsRows.end() && (row->required == None || hasFlag(held, row->required)))
        return {};
    return refuse(FsErrorCode::NotPermitted);
}

FsResult<void> checkOpenRights(DescriptorFlags parent, OpenFlags open, DescriptorFlags child) noexcept
{
    // wasmtime's order: what the parent may not grant is NotPermitted before the flags themselves
    // are judged.
    auto const changesEntries = hasFlag(open, OpenFlags::Create) || hasFlag(open, OpenFlags::Truncate)
                                || hasFlag(open, OpenFlags::Exclusive) || hasFlag(open, OpenFlags::OwnerOnly);
    if (hasFlag(child, Read) && !hasFlag(parent, Read))
        return refuse(FsErrorCode::NotPermitted);
    auto const needsMutate = hasFlag(child, Write) || hasFlag(child, MutateDirectory) || changesEntries;
    if (needsMutate && !hasFlag(parent, MutateDirectory))
        return refuse(FsErrorCode::NotPermitted);
    if (hasFlag(open, OpenFlags::Directory) && changesEntries)
        return refuse(FsErrorCode::Invalid);
    // Owner-only is a property of a creation this call surely made: without Create the caller would
    // get whatever is there, and without Exclusive an ACL stripped, or a name unlinked on failure,
    // could be another's.
    if (hasFlag(open, OpenFlags::OwnerOnly)
        && (!hasFlag(open, OpenFlags::Create) || !hasFlag(open, OpenFlags::Exclusive)))
        return refuse(FsErrorCode::Invalid);
    if (hasFlag(open, OpenFlags::Directory) && hasFlag(child, Write))
        return refuse(FsErrorCode::IsDirectory);
    return {};
}

DescriptorFlags grantedFlags(DescriptorType type, DescriptorFlags requested) noexcept
{
    if (type == DescriptorType::Directory)
        return requested;
    return static_cast<DescriptorFlags>(static_cast<std::uint8_t>(requested)
                                        & ~static_cast<std::uint8_t>(MutateDirectory));
}

} // namespace core::fs
