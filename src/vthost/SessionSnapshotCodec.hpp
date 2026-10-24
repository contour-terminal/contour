// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The snapshot file's byte format. It is read across releases (the drift restart writes it with the old
/// daemon and reads it with the new one), so it follows phase 1's append-only rule: a record grows only by
/// appending fields inside its `blob{…}`, and a reader skips what it does not know.
///
/// ```
/// File     := Magic Version:varint BodyLength:varint ChecksumLo:u32 ChecksumHi:u32 Body
/// Magic    := the 8 bytes "CTRSNAP\n"
/// Body     := TakenAt:svarint(unix seconds) WindowCount:varint Window{WindowCount}
/// Window   := blob{ Identity? ActiveTab:varint TabCount:varint Tab{TabCount} }
/// Tab      := blob{ Identity? Title:string? Color:u32(0xRRGGBB)? ActiveLeaf:varint Zoom:u8 Node }
/// Node     := blob{ Split:u8 ( Ratio:varint(millionths) Node Node | Leaf ) }
/// Leaf     := Identity? Cwd:string(UTF-8)? Profile:string? Foreground:string? CommandLine:string?
///             History:blob(encodePdu(Delta))?
/// Identity := 16 raw bytes, Label:string?
/// X?       := u8 0 (absent) | u8 1 then X
/// ```
///
/// The checksum (FNV-1a, 64 bit, split into two little-endian halves) covers `Body` only.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <vthost/SessionSnapshot.hpp>

namespace vthost
{

/// Why a snapshot file could not be read.
enum class SnapshotError : std::uint8_t
{
    BadMagic = 0,      ///< The file does not start with the snapshot magic.
    UnsupportedFormat, ///< A format version this build does not know, i.e. written by a newer release.
    Truncated,         ///< The file ends before its declared body does.
    ChecksumMismatch,  ///< The body is not what was written.
    Malformed,         ///< The body decodes to something no encoder writes, or exceeds a limit.
    TooLarge,          ///< The file exceeds MaxSnapshotFileBytes.
};

/// @param error The failure.
/// @return A lower-case phrase for a log line.
[[nodiscard]] constexpr std::string_view describe(SnapshotError error) noexcept
{
    switch (error)
    {
        case SnapshotError::BadMagic: return "not a snapshot file";
        case SnapshotError::UnsupportedFormat: return "unsupported snapshot format";
        case SnapshotError::Truncated: return "truncated snapshot";
        case SnapshotError::ChecksumMismatch: return "snapshot checksum mismatch";
        case SnapshotError::Malformed: return "malformed snapshot";
        case SnapshotError::TooLarge: return "snapshot too large";
    }
    return "unknown snapshot error";
}

/// The format version this build writes. A step only for a change an older reader would misread.
inline constexpr std::uint64_t SnapshotFormatVersion = 1;
/// The largest snapshot file written or accepted.
inline constexpr std::size_t MaxSnapshotFileBytes = std::size_t { 64 } * 1024 * 1024;
inline constexpr std::size_t MaxSnapshotWindows = 64;
inline constexpr std::size_t MaxTabsPerWindow = 512;
inline constexpr std::size_t MaxLeavesPerTab = 256;
/// The deepest nesting of splits along one path.
inline constexpr std::size_t MaxPaneDepth = 32;

/// A decoded file and what it lost on the way.
struct DecodedSnapshot
{
    WorkspaceSnapshot snapshot;
    std::size_t droppedHistories = 0; ///< Panes whose history frame did not decode, restored without it.
};

/// FNV-1a, 64 bit.
/// @param bytes The bytes to hash.
/// @return Their hash.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::span<std::byte const> bytes) noexcept
{
    auto hash = std::uint64_t { 0xcbf29ce484222325ULL };
    for (auto const byte: bytes)
    {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/// Encodes @p snapshot as a snapshot file.
/// @param snapshot The workspace to write.
/// @return The file's bytes.
[[nodiscard]] std::vector<std::byte> encodeSnapshot(WorkspaceSnapshot const& snapshot);

/// Decodes a snapshot file.
/// @param file The file's bytes.
/// @return The workspace, or the first thing wrong with the file.
[[nodiscard]] std::expected<DecodedSnapshot, SnapshotError> decodeSnapshot(std::span<std::byte const> file);

} // namespace vthost
