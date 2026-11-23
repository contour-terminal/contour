// SPDX-License-Identifier: Apache-2.0
#include <vtpty/Win32OpenBeneath.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

#include <winternl.h>

namespace vtpty
{

namespace
{
    /// How NtCreateFile opens one BeneathEntry.
    struct EntryRow
    {
        BeneathEntry entry;
        ULONG disposition;
        ULONG options;
    };

    /// One row per BeneathEntry, in its order. Every row opens a reparse point itself
    /// (FILE_OPEN_REPARSE_POINT), never what it names.
    constexpr auto EntryRows = std::to_array<EntryRow>({
        { .entry = BeneathEntry::DirectoryCreatedIfMissing,
          .disposition = FILE_OPEN_IF,
          .options = FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT },
    });
    // A row per enumerator, each at its enumerator's index: a new BeneathEntry is a new row, here.
    static_assert(EntryRows.size() == std::to_underlying(BeneathEntry::Last) + std::size_t { 1 });
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, EntryRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(EntryRows.at(index).entry) == index;
                                      }),
                  "EntryRows is in BeneathEntry order");

    /// @param name What openBeneath() was asked to open.
    /// @return Whether @p name is one component: not empty, and no separator, so that NtCreateFile neither
    ///         reopens the parent itself nor walks into a subdirectory.
    [[nodiscard]] bool isOneComponent(std::wstring_view name) noexcept
    {
        return !name.empty() && !name.contains(L'\\') && !name.contains(L'/');
    }

    /// The ntdll functions openBeneath() calls, looked up once.
    struct NtFunctions
    {
        decltype(&::NtCreateFile) createFile;
        decltype(&::RtlNtStatusToDosError) statusToDosError;
    };

    /// @return ntdll's functions; either null where this Windows has none.
    [[nodiscard]] NtFunctions const& ntFunctions() noexcept
    {
        static auto const functions = NtFunctions {
            .createFile = ntdllFunction<decltype(&::NtCreateFile)>("NtCreateFile"),
            .statusToDosError = ntdllFunction<decltype(&::RtlNtStatusToDosError)>("RtlNtStatusToDosError"),
        };
        return functions;
    }
} // namespace

std::expected<UniqueHandle, DWORD> openBeneath(HANDLE parent,
                                               std::wstring_view name,
                                               BeneathEntry entry,
                                               DWORD access)
{
    if (parent == nullptr || !isOneComponent(name))
        return std::unexpected(DWORD { ERROR_ACCESS_DENIED });
    auto const& nt = ntFunctions();
    if (nt.createFile == nullptr || nt.statusToDosError == nullptr)
        return std::unexpected(DWORD { ERROR_CALL_NOT_IMPLEMENTED });
    // A UNICODE_STRING counts its bytes in a USHORT.
    if (name.size() * sizeof(wchar_t) > std::numeric_limits<USHORT>::max())
        return std::unexpected(DWORD { ERROR_FILENAME_EXCED_RANGE });

    auto buffer = std::wstring { name };
    auto objectName = UNICODE_STRING {
        .Length = static_cast<USHORT>(buffer.size() * sizeof(wchar_t)),
        .MaximumLength = static_cast<USHORT>(buffer.size() * sizeof(wchar_t)),
        .Buffer = buffer.data(),
    };
    // No OBJ_INHERIT: the handle is not inheritable. No security descriptor: a created directory inherits
    // its parent's ACL, the analogue of mkdir 0777 under the umask.
    auto attributes = OBJECT_ATTRIBUTES {
        .Length = sizeof(OBJECT_ATTRIBUTES),
        .RootDirectory = parent,
        .ObjectName = &objectName,
        .Attributes = OBJ_CASE_INSENSITIVE,
        .SecurityDescriptor = nullptr,
        .SecurityQualityOfService = nullptr,
    };
    auto const& row = EntryRows.at(std::to_underlying(entry));
    auto status = IO_STATUS_BLOCK {};
    auto handle = HANDLE { nullptr };
    // FILE_SYNCHRONOUS_IO_NONALERT requires SYNCHRONIZE.
    auto const result = nt.createFile(&handle,
                                      access | SYNCHRONIZE,
                                      &attributes,
                                      &status,
                                      nullptr,
                                      FILE_ATTRIBUTE_NORMAL,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      row.disposition,
                                      row.options,
                                      nullptr,
                                      0);
    if (result < 0)
        return std::unexpected(DWORD { nt.statusToDosError(result) });
    return UniqueHandle { handle };
}

bool isNameSurrogate(HANDLE handle) noexcept
{
    auto info = FILE_ATTRIBUTE_TAG_INFO {};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) == FALSE)
        return true;
    return (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
           && IsReparseTagNameSurrogate(info.ReparseTag);
}

} // namespace vtpty
