// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native API the Windows descriptors open and name files through: `NtCreateFile` relative to a
/// held directory handle, which is the only way Windows resolves one name beneath a handle rather
/// than a whole path from a drive, and `NtSetInformationFile`, which renames and links an open file
/// to a name beneath another held handle. Private to the fs module, never included by a public
/// header, and the reason the module links `ntdll`.
///
/// `<winternl.h>` declares `NtCreateFile` but neither `NtSetInformationFile` nor the information
/// classes it takes beyond the first, which live in the driver kit's `<ntifs.h>`; the few used here
/// are spelled out below with their documented values, and `NtSetInformationFile` is resolved from
/// the `ntdll` every process has mapped, as `core::net`'s IOCP backend resolves it.

#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// <ntstatus.h> defines every STATUS_ code, and <windows.h> defines some of them again unless it is
// told not to: the documented order is this one. Every core-cpp target builds with
// WIN32_LEAN_AND_MEAN, under which <windows.h> no longer declares NTSTATUS; <winternl.h> does, and
// NtCreateFile with it.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

namespace core::fs::detail
{

/// `OBJ_DONT_REPARSE` (Windows 10 1803): the object manager refuses any reparse point on the way to
/// the name, so a junction can never be crossed even if a name held a separator. Spelled here
/// because an SDK older than 10.0.17134 lacks the name; the value is the documented one.
#ifdef OBJ_DONT_REPARSE
inline constexpr ULONG DontReparse = OBJ_DONT_REPARSE;
#else
inline constexpr ULONG DontReparse = 0x0000'1000UL;
#endif

/// How an open treats a reparse point it names.
enum class ReparseHandling : std::uint8_t
{
    /// It opens the point itself (`FILE_OPEN_REPARSE_POINT`) and refuses any met on the way to it
    /// (`OBJ_DONT_REPARSE`), so the kernel follows no link: what every open by a name does.
    OpenThePoint,
    /// It lets the file system's filters act on the point, as an ordinary open does, but still
    /// refuses any reparse the object manager would follow (`OBJ_DONT_REPARSE`): a filter serving its
    /// own tag (a cloud placeholder, a deduplicated file) consumes the reparse itself, while a point
    /// swapped for a mount point since it was checked answers `STATUS_REPARSE_POINT_ENCOUNTERED`,
    /// which the callers refuse as `NotPermitted`, rather than being followed to wherever it names. Only for
    /// opening again, by its handle and with no name, a point already known to name nothing else.
    /// Before Windows 10 1803, which lacks the flag, such an open is refused.
    ThroughFilters,
};

/// What `NtCreateFile` is asked for, besides the name.
struct NtOpenRequest
{
    ACCESS_MASK access = 0; ///< The access wanted; `SYNCHRONIZE` is always added.
    ULONG disposition = 0;  ///< `FILE_OPEN`, `FILE_CREATE`, ...
    ULONG options = 0;      ///< `FILE_DIRECTORY_FILE`, ...; synchronous I/O is always added.
    ReparseHandling reparse = ReparseHandling::OpenThePoint; ///< What a reparse point it names is.
    /// What an object this open creates is given as its owner and DACL, by `NtCreateFile` itself
    /// (`OBJECT_ATTRIBUTES::SecurityDescriptor`), so it is never anyone else's for a moment; null for
    /// what the system gives (the inherited DACL). An object that already exists keeps its own.
    PSECURITY_DESCRIPTOR security = nullptr;
};

/// How every open identifies this process to a server it reaches (a named pipe, a share): never
/// beyond identification, so no peer can act as this process with its token.
inline constexpr auto IdentificationOnly = SECURITY_QUALITY_OF_SERVICE {
    .Length = sizeof(SECURITY_QUALITY_OF_SERVICE),
    .ImpersonationLevel = SecurityIdentification,
    .ContextTrackingMode = SECURITY_STATIC_TRACKING,
    .EffectiveOnly = TRUE,
};

/// Opens @p name beneath @p directory, with `OBJ_DONT_REPARSE` where the system knows it and
/// @c IdentificationOnly. Opening the point itself before Windows 10 1803, where that flag is
/// `STATUS_INVALID_PARAMETER`, the open is made again without it, as Rust's standard library does;
/// the name is one component either way, and the point itself is what is opened. Opening through
/// filters is never made without it.
/// @param directory A held handle the name is relative to: a directory, or the object itself for
///        an empty name.
/// @param name One component in UTF-16; empty opens @p directory itself again.
/// @param request What to ask for.
/// @return The handle, owned by the caller; the NTSTATUS, classified, where it failed.
[[nodiscard]] inline FsResult<UniqueHandle> ntOpenRelative(HANDLE directory,
                                                           std::wstring const& name,
                                                           NtOpenRequest const& request)
{
    auto const openThePoint = request.reparse == ReparseHandling::OpenThePoint;
    // Through filters only by a handle: a name could be anything by now.
    if (!openThePoint && !name.empty())
        return std::unexpected { FsError { .code = FsErrorCode::Invalid,
                                           .systemCode = 0,
                                           .context = "NtCreateFile through filters by name" } };
    auto const bytes = name.size() * sizeof(wchar_t);
    if (bytes > std::numeric_limits<USHORT>::max())
        return std::unexpected { FsError {
            .code = FsErrorCode::NameTooLong, .systemCode = 0, .context = {} } };
    auto unicodeName = UNICODE_STRING { .Length = static_cast<USHORT>(bytes),
                                        .MaximumLength = static_cast<USHORT>(bytes),
                                        .Buffer = const_cast<PWSTR>(name.c_str()) };
    auto qualityOfService = IdentificationOnly;
    auto const open = [&](ULONG attributeFlags) {
        auto attributes = OBJECT_ATTRIBUTES { .Length = sizeof(OBJECT_ATTRIBUTES),
                                              .RootDirectory = directory,
                                              .ObjectName = &unicodeName,
                                              .Attributes = OBJ_CASE_INSENSITIVE | attributeFlags,
                                              .SecurityDescriptor = request.security,
                                              .SecurityQualityOfService = &qualityOfService };
        auto status = IO_STATUS_BLOCK {};
        auto handle = HANDLE {};
        auto const result =
            ::NtCreateFile(&handle,
                           request.access | SYNCHRONIZE,
                           &attributes,
                           &status,
                           nullptr,
                           FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           request.disposition,
                           request.options | FILE_SYNCHRONOUS_IO_NONALERT
                               | (openThePoint ? ULONG { FILE_OPEN_REPARSE_POINT } : ULONG { 0 }),
                           nullptr,
                           0);
        return std::pair { result, handle };
    };
    auto [result, handle] = open(DontReparse);
    if (result == STATUS_INVALID_PARAMETER && openThePoint)
        std::tie(result, handle) = open(0);
    if (result < 0)
        return std::unexpected { fsErrorFromNtStatus(std::int64_t { result }, "NtCreateFile") };
    return UniqueHandle { handle };
}

/// The `NtSetInformationFile` classes that give an open file a name beneath a held directory.
inline constexpr ULONG FileRenameInformationClass = 10;   ///< `FileRenameInformation`.
inline constexpr ULONG FileLinkInformationClass = 11;     ///< `FileLinkInformation`.
inline constexpr ULONG FileRenameInformationExClass = 65; ///< `FileRenameInformationEx` (Windows 10 1709).

/// The flags of `FileRenameInformationEx`; `FileRenameInformation` and `FileLinkInformation` read the
/// low byte as their `ReplaceIfExists`.
inline constexpr ULONG RenameReplaceIfExists = 0x01;
inline constexpr ULONG RenamePosixSemantics = 0x02; ///< The name goes at once, even while the target is open.
inline constexpr ULONG RenameIgnoreReadOnlyAttribute = 0x40;

/// The head `FILE_RENAME_INFORMATION` and `FILE_LINK_INFORMATION` share: the flags (a `BOOLEAN`
/// `ReplaceIfExists` in their low byte before the `Ex` classes), the directory the new name is
/// relative to, and the name's length in bytes. The name follows the length, before the padding
/// that rounds the head up to a pointer's alignment.
struct NameInformationHead
{
    ULONG flags = 0;
    HANDLE rootDirectory = nullptr;
    ULONG fileNameLength = 0;
};

/// Where the name begins in a `FILE_RENAME_INFORMATION` or `FILE_LINK_INFORMATION`.
inline constexpr std::size_t NameInformationNameOffset =
    offsetof(NameInformationHead, fileNameLength) + sizeof(ULONG);

/// `NtSetInformationFile`, with the class taken as the `ULONG` it is passed as, since
/// `<winternl.h>`'s `FILE_INFORMATION_CLASS` has none of the values used here.
using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);

/// @return `NtSetInformationFile`, resolved once; null where `ntdll` lacks it, which no Windows does.
[[nodiscard]] inline NtSetInformationFileFn ntSetInformationFile() noexcept
{
    static auto const entry = []() noexcept -> NtSetInformationFileFn {
        auto* const ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
            return nullptr;
        // reinterpret_cast straight from FARPROC, never through `void*`: the documented way to use
        // GetProcAddress (see core::net's WaitCompletionPacket.cpp).
        return reinterpret_cast<NtSetInformationFileFn>(::GetProcAddress(ntdll, "NtSetInformationFile"));
    }();
    return entry;
}

/// Gives @p handle's object the name @p name beneath @p directory: renames it, or links it there.
/// @param handle The object, open.
/// @param informationClass @c FileRenameInformationClass, @c FileRenameInformationExClass or
///        @c FileLinkInformationClass.
/// @param flags What the class takes as its flags.
/// @param directory A held directory handle on the same volume.
/// @param name One component in UTF-16.
/// @return Nothing; the NTSTATUS, classified, where it failed.
[[nodiscard]] inline FsResult<void> ntSetName(
    HANDLE handle, ULONG informationClass, ULONG flags, HANDLE directory, std::wstring const& name)
{
    auto const bytes = name.size() * sizeof(wchar_t);
    if (bytes > std::numeric_limits<ULONG>::max() - NameInformationNameOffset)
        return std::unexpected { FsError {
            .code = FsErrorCode::NameTooLong, .systemCode = 0, .context = {} } };
    auto const head = NameInformationHead { .flags = flags,
                                            .rootDirectory = directory,
                                            .fileNameLength = static_cast<ULONG>(bytes) };
    // The head first, then the name over its padding.
    auto buffer = std::vector<std::byte>(std::max(sizeof(head), NameInformationNameOffset + bytes));
    std::memcpy(buffer.data(), &head, sizeof(head));
    std::memcpy(buffer.data() + NameInformationNameOffset, name.data(), bytes);
    auto const setInformation = ntSetInformationFile();
    if (setInformation == nullptr)
        return std::unexpected { FsError {
            .code = FsErrorCode::Unsupported, .systemCode = 0, .context = "NtSetInformationFile" } };
    auto status = IO_STATUS_BLOCK {};
    auto const result =
        setInformation(handle, &status, buffer.data(), static_cast<ULONG>(buffer.size()), informationClass);
    if (result < 0)
        return std::unexpected { fsErrorFromNtStatus(std::int64_t { result }, "NtSetInformationFile") };
    return {};
}

/// @param text A name in UTF-8.
/// @return It in UTF-16; `IllegalByteSequence` where it is no UTF-8, `NameTooLong` past what the
///         conversion takes.
[[nodiscard]] inline FsResult<std::wstring> utf16Of(std::string const& text)
{
    if (text.empty())
        return std::wstring {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected { FsError {
            .code = FsErrorCode::NameTooLong, .systemCode = 0, .context = {} } };
    auto const size = static_cast<int>(text.size());
    auto const length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (length <= 0)
        return std::unexpected { FsError {
            .code = FsErrorCode::IllegalByteSequence, .systemCode = 0, .context = {} } };
    auto wide = std::wstring(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, wide.data(), length);
    return wide;
}

/// @param text A name in UTF-16, as the file system holds it.
/// @return It in UTF-8; `IllegalByteSequence` for an unpaired surrogate, which UTF-8 cannot spell.
[[nodiscard]] inline FsResult<std::string> utf8Of(std::wstring const& text)
{
    if (text.empty())
        return std::string {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected { FsError {
            .code = FsErrorCode::NameTooLong, .systemCode = 0, .context = {} } };
    auto const size = static_cast<int>(text.size());
    auto const length =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return std::unexpected { FsError {
            .code = FsErrorCode::IllegalByteSequence, .systemCode = 0, .context = {} } };
    auto narrow = std::string(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, narrow.data(), length, nullptr, nullptr);
    return narrow;
}

} // namespace core::fs::detail
