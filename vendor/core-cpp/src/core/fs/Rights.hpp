// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which rights each descriptor operation needs, as one table every implementation enforces, and the
/// rule for the rights a child opened by `openAt` may carry.

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>

#include <cstdint>
#include <span>

namespace core::fs
{

/// An @c IDescriptor operation, as @c rightsTable names it.
enum class Operation : std::uint8_t
{
    Read,              ///< @c IDescriptor::read.
    Write,             ///< @c IDescriptor::write.
    Append,            ///< @c IDescriptor::append.
    Advise,            ///< @c IDescriptor::advise.
    Sync,              ///< @c IDescriptor::sync and @c IDescriptor::syncData.
    SetSize,           ///< @c IDescriptor::setSize.
    SetTimes,          ///< @c IDescriptor::setTimes.
    Stat,              ///< @c IDescriptor::stat and @c IDescriptor::type.
    MetadataHash,      ///< @c IDescriptor::metadataHash.
    ReadDirectory,     ///< @c IDescriptor::readDirectory.
    OpenAt,            ///< @c IDescriptor::openAt, before @c checkOpenRights.
    CreateDirectoryAt, ///< @c IDescriptor::createDirectoryAt.
    StatAt,            ///< @c IDescriptor::statAt.
    SetTimesAt,        ///< @c IDescriptor::setTimesAt.
    LinkAt,            ///< @c IDescriptor::linkAt, on both directories.
    ReadlinkAt,        ///< @c IDescriptor::readlinkAt.
    RemoveDirectoryAt, ///< @c IDescriptor::removeDirectoryAt.
    RenameAt,          ///< @c IDescriptor::renameAt, on both directories.
    SymlinkAt,         ///< @c IDescriptor::symlinkAt.
    UnlinkFileAt,      ///< @c IDescriptor::unlinkFileAt.
    MetadataHashAt,    ///< @c IDescriptor::metadataHashAt.
    Last,              ///< Not an operation: the number of them, so a test can walk the table.
};

/// One row of @c rightsTable.
struct RightsRow
{
    Operation operation;      ///< The operation.
    DescriptorFlags required; ///< The descriptor must carry one of these; `None` needs nothing.
};

/// @return One row per @c Operation, in its order: what the descriptor it is called on must carry.
[[nodiscard]] CORE_FS_API std::span<RightsRow const> rightsTable() noexcept;

/// Checks @p held against @p operation's row of @c rightsTable.
/// @param held The flags of the descriptor the operation is called on.
/// @param operation The operation.
/// @return Nothing, or `NotPermitted`.
[[nodiscard]] CORE_FS_API FsResult<void> checkRights(DescriptorFlags held, Operation operation) noexcept;

/// Checks what a child opened by @c IDescriptor::openAt may carry, against its parent directory:
/// - `Read` needs the parent's `Read`;
/// - `Write` or `MutateDirectory`, and `Create`, `Truncate`, `Exclusive` or `OwnerOnly`, need the
///   parent's `MutateDirectory`;
/// - the three sync flags are a mode, not a right, and need nothing;
/// - then, as wasmtime checks them only once the rights hold: `Directory` with `Create`,
///   `Truncate`, `Exclusive` or `OwnerOnly` is `Invalid`, and a directory never carries `Write`
///   (`IsDirectory`); `OwnerOnly` without both `Create` and `Exclusive` is `Invalid`.
/// @param parent The flags of the directory @c openAt is called on.
/// @param open How the child is opened.
/// @param child The flags the child asks for.
/// @return Nothing, or `NotPermitted`, `IsDirectory` or `Invalid`.
[[nodiscard]] CORE_FS_API FsResult<void> checkOpenRights(DescriptorFlags parent,
                                                         OpenFlags open,
                                                         DescriptorFlags child) noexcept;

/// The flags a descriptor opened with @p requested carries once its object is known: only a
/// directory carries `MutateDirectory`, so a file asked for it gets the rest, as in wasmtime.
/// Together with @c checkOpenRights' rule that a directory never carries `Write`, this is what lets
/// @c Operation::SetTimes need "one of `Write` and `MutateDirectory`": a file has only the first to
/// offer, a directory only the second.
/// @param type What the new descriptor refers to.
/// @param requested The flags `openAt` was asked for, already held to @c checkOpenRights.
/// @return The flags it carries.
[[nodiscard]] CORE_FS_API DescriptorFlags grantedFlags(DescriptorType type,
                                                       DescriptorFlags requested) noexcept;

} // namespace core::fs
