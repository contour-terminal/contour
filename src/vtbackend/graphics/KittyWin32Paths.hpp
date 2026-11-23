// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Windows rules of the kitty file-medium policy that are about spellings and attribute bits rather than
/// about any file: which path forms and components a command may name, which drives are read, how the final
/// path Windows reports reads, and which attributes refuse an entry. Platform-independent and pure, so the
/// rules are tested everywhere; the Windows source (Win32MediumSource) applies them to what Windows answers.

#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace vtbackend::kitty_graphics::win32
{

/// The file attribute bits the rules judge: the values of the SDK's `FILE_ATTRIBUTE_*`, spelled here so the
/// rules need no Windows header. Win32MediumSource asserts that they agree.
namespace attribute
{
    inline constexpr uint32_t Hidden = 0b0000'0010;                      ///< FILE_ATTRIBUTE_HIDDEN
    inline constexpr uint32_t System = 0b0000'0100;                      ///< FILE_ATTRIBUTE_SYSTEM
    inline constexpr uint32_t Directory = 0b0001'0000;                   ///< FILE_ATTRIBUTE_DIRECTORY
    inline constexpr uint32_t Device = 0b0100'0000;                      ///< FILE_ATTRIBUTE_DEVICE
    inline constexpr uint32_t ReparsePoint = 0b0100'0000'0000;           ///< FILE_ATTRIBUTE_REPARSE_POINT
    inline constexpr uint32_t Offline = 0b0001'0000'0000'0000;           ///< FILE_ATTRIBUTE_OFFLINE
    inline constexpr uint32_t RecallOnOpen = 0b0100'0000'0000'0000'0000; ///< ..._RECALL_ON_OPEN
    inline constexpr uint32_t RecallOnDataAccess =
        0b0100'0000'0000'0000'0000'0000; ///< ..._RECALL_ON_DATA_ACCESS
} // namespace attribute

/// The drive types `GetDriveTypeW` answers that the rules read from: the SDK's `DRIVE_*` values.
namespace drive_type
{
    inline constexpr uint32_t Fixed = 3;   ///< DRIVE_FIXED
    inline constexpr uint32_t RamDisk = 6; ///< DRIVE_RAMDISK
} // namespace drive_type

/// What an opened entry below a root is meant to be.
enum class EntryRole : uint8_t
{
    Directory = 0, ///< A directory on the way to the file.
    File = 1,      ///< The file read.
};

/// Judges the spelling of a path a command names, before anything decodes or opens it: a drive-absolute path
/// (`X:\...` or `X:/...`) whose every component names a file and nothing else. Refused:
/// - UNC (`\\server\share`), the device and NT namespaces (`\\?\`, `\\.\`, `\??\`): UnsupportedDrive;
/// - anything not drive-absolute, a POSIX path from WSL or MSYS included: OutsideRoots, never mapped;
/// - a component checkComponent() refuses.
/// @param path The path as the command sent it, UTF-8.
/// @return Nothing when it may be judged further; the refusal otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkPathSpelling(std::string_view path) noexcept;

/// Judges one path component: refused as ReservedName when Windows would read it as something other than a
/// file name -- `.` or `..`, an alternate data stream (`:`), a wildcard or quote (`* ? " < > |`), a control
/// character, a trailing dot or space (which Windows strips), or a device name (`CON`, `NUL`, `COM1`, ...,
/// with any extension). Hidden names are the policy's to judge, not this.
/// @param component One component, UTF-8, without separators.
/// @return Nothing when it names a file; ReservedName otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkComponent(std::string_view component) noexcept;

/// Judges a drive by its type: only fixed and RAM disks are read. A network drive would stall the terminal
/// and tell the server who read what; a removable or optical one may stall, prompt, or be gone.
/// @param driveType What `GetDriveTypeW` answers for the drive's root.
/// @return Nothing for a fixed or RAM disk; UnsupportedDrive otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkDriveType(uint32_t driveType) noexcept;

/// Reads what `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)` answers as a drive path:
/// the `\\?\` prefix removed. A share (`\\?\UNC\...`) or a volume without a drive letter (`\\?\Volume{...}`)
/// is refused.
/// @param finalPath The final path, as reported.
/// @return The drive path (`X:\...`); UnsupportedDrive otherwise.
[[nodiscard]] std::expected<std::wstring, MediaRefusal> drivePathOfFinalPath(std::wstring_view finalPath);

/// Judges the attributes of an entry opened below a root (never followed: a reparse point is opened as
/// itself). Refused, by the first rule that matches: a reparse point (SymbolicLink), a hidden or system
/// entry (HiddenComponent), an offline or recall-on-access one, which would fetch it from elsewhere
/// (Unreadable), and for the file a directory or a device (NotRegularFile); for a directory, one without the
/// directory bit (NotRegularFile).
/// @param attributes The entry's `FILE_ATTRIBUTE_*` bits.
/// @param role What the entry is meant to be.
/// @return Nothing when it may be used as @p role; the refusal otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkAttributes(uint32_t attributes, EntryRole role) noexcept;

} // namespace vtbackend::kitty_graphics::win32
