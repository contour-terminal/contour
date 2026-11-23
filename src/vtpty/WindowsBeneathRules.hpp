// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/BeneathRead.hpp>

#include <array>
#include <cstdint>

// What a Windows entry is to a read beneath a directory, as a pure rule over the facts a handle reports. The
// Win32 values are spelled here as their documented numbers, so this header needs no Windows headers and is
// compiled and tested on every platform; the Win32 side asserts each against the SDK macro of the same
// meaning.

namespace vtpty
{

/// The FILE_ATTRIBUTE_* bits the rule reads, as Win32 documents their values.
namespace WindowsAttribute
{
    inline constexpr uint32_t Directory = 0x0000'0010;          ///< FILE_ATTRIBUTE_DIRECTORY
    inline constexpr uint32_t ReparsePoint = 0x0000'0400;       ///< FILE_ATTRIBUTE_REPARSE_POINT
    inline constexpr uint32_t Offline = 0x0000'1000;            ///< FILE_ATTRIBUTE_OFFLINE
    inline constexpr uint32_t RecallOnOpen = 0x0004'0000;       ///< FILE_ATTRIBUTE_RECALL_ON_OPEN
    inline constexpr uint32_t RecallOnDataAccess = 0x0040'0000; ///< FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
} // namespace WindowsAttribute

/// IsReparseTagNameSurrogate()'s bit: junctions, symlinks, mount points.
inline constexpr uint32_t WindowsNameSurrogateBit = 0x2000'0000;

/// A reparse tag that marks how a real file or directory is stored, not where it leads, and the mask of the
/// bits that may vary within its family.
struct StorageReparseTag
{
    uint32_t tag;  ///< The tag, its variable bits zero.
    uint32_t mask; ///< The bits ignored when comparing (IO_REPARSE_TAG_CLOUD_MASK for the cloud family).
};

/// The storage tags a read accepts: one row per family, a new family a new row.
inline constexpr auto AcceptedStorageReparseTags = std::to_array<StorageReparseTag>({
    { .tag = 0x8000'0013, .mask = 0 },           // IO_REPARSE_TAG_DEDUP
    { .tag = 0x8000'0017, .mask = 0 },           // IO_REPARSE_TAG_WOF
    { .tag = 0x9000'001A, .mask = 0x0000'F000 }, // IO_REPARSE_TAG_CLOUD .. CLOUD_F
});

/// What GetFileType() says a handle is, as far as a read cares.
enum class WindowsHandleType : uint8_t
{
    Other = 0, ///< A pipe, a character device, unknown.
    Disk,      ///< FILE_TYPE_DISK.
};

/// What a Windows handle tells of the entry it opened.
struct WindowsEntryFacts
{
    uint32_t attributes = 0; ///< FILE_ATTRIBUTE_* bits.
    uint32_t reparseTag = 0; ///< Its tag, where attributes holds ReparsePoint.
    WindowsHandleType handleType = WindowsHandleType::Other; ///< GetFileType()'s answer.
};

/// @param facts What a handle tells of its entry.
/// @return What the entry is to a read beneath a directory (Decision 3): Symlink for a name surrogate;
///         Other for a reparse tag not in AcceptedStorageReparseTags, a non-disk handle, or data not on disk
///         (Offline, RecallOnOpen, RecallOnDataAccess) on a non-directory; Directory; else Regular.
[[nodiscard]] FileKind windowsFileKindOf(WindowsEntryFacts facts) noexcept;

/// @param fileTime A FILETIME as one 64-bit count of 100 ns since 1601-01-01 UTC.
/// @return Milliseconds since the Unix epoch; 0 for any time before it.
[[nodiscard]] uint64_t unixMsOfFileTime(uint64_t fileTime) noexcept;

} // namespace vtpty
