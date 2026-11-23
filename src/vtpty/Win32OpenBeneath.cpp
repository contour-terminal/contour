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
        // No FILE_OPEN_NO_RECALL: a directory is walked, not read, and walking a cloud placeholder directory
        // is accepted (Decision 3); asking for no recall could make its children look missing.
        { .entry = BeneathEntry::Directory,
          .disposition = FILE_OPEN,
          .options = FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT },
        // FILE_OPEN_NO_RECALL: looking at a file must not fetch a placeholder's data from remote storage
        // before its attributes tell it is not on disk.
        { .entry = BeneathEntry::Entry,
          .disposition = FILE_OPEN,
          .options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_OPEN_NO_RECALL },
    });
    // A row per enumerator, each at its enumerator's index: a new BeneathEntry is a new row, here.
    static_assert(EntryRows.size() == std::to_underlying(BeneathEntry::Last) + std::size_t { 1 });
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, EntryRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(EntryRows.at(index).entry) == index;
                                      }),
                  "EntryRows is in BeneathEntry order");
    static_assert(std::ranges::all_of(EntryRows,
                                      [](EntryRow const& row) {
                                          return (row.options & FILE_OPEN_REPARSE_POINT) != 0
                                                 && (row.options & FILE_NON_DIRECTORY_FILE) == 0;
                                      }),
                  "Every entry is opened itself, never a reparse point's target, and as a directory or not");
    static_assert((EntryRows.at(std::to_underlying(BeneathEntry::Entry)).options & FILE_OPEN_NO_RECALL) != 0,
                  "Looking at a file to be read recalls nothing from remote storage");

    /// @param name What openBeneath() was asked to open.
    /// @return Whether @p name is one component: not empty, and no separator, so that NtCreateFile neither
    ///         reopens the parent itself nor walks into a subdirectory.
    [[nodiscard]] bool isOneComponent(std::wstring_view name) noexcept
    {
        return !name.empty() && !name.contains(L'\\') && !name.contains(L'/');
    }

    /// How reopenHeld() opens the object it is given: itself, never a reparse point's target; without
    /// recalling data held in remote storage; read front to back; never a directory, so that no handle a
    /// read holds (the parent's included) can be turned into one that lists or reads a directory.
    constexpr auto ReopenOptions =
        ULONG { FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_OPEN_NO_RECALL
                | FILE_SEQUENTIAL_ONLY | FILE_NON_DIRECTORY_FILE };

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

    /// Opens @p name relative to @p root (NtCreateFile with a RootDirectory), shared for read, write and
    /// delete, not inheritable.
    /// @param root An open file or directory; a null one is refused (ERROR_ACCESS_DENIED).
    /// @param name What to open beneath @p root; empty for @p root's own object.
    /// @param access The access asked for; SYNCHRONIZE is added.
    /// @param disposition NtCreateFile's CreateDisposition.
    /// @param options NtCreateFile's CreateOptions; FILE_SYNCHRONOUS_IO_NONALERT among them.
    /// @return The handle; ERROR_ACCESS_DENIED for a null @p root; or the Win32 error the NTSTATUS maps to.
    [[nodiscard]] std::expected<UniqueHandle, DWORD> createRelative(
        HANDLE root, std::wstring_view name, DWORD access, ULONG disposition, ULONG options)
    {
        // With no RootDirectory, NtCreateFile would read the name as an absolute object path.
        if (root == nullptr)
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
            .RootDirectory = root,
            .ObjectName = &objectName,
            .Attributes = OBJ_CASE_INSENSITIVE,
            .SecurityDescriptor = nullptr,
            .SecurityQualityOfService = nullptr,
        };
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
                                          disposition,
                                          options,
                                          nullptr,
                                          0);
        if (result < 0)
            return std::unexpected(DWORD { nt.statusToDosError(result) });
        return UniqueHandle { handle };
    }
} // namespace

std::expected<UniqueHandle, DWORD> openBeneath(HANDLE parent,
                                               std::wstring_view name,
                                               BeneathEntry entry,
                                               DWORD access)
{
    if (!isOneComponent(name))
        return std::unexpected(DWORD { ERROR_ACCESS_DENIED });
    auto const& row = EntryRows.at(std::to_underlying(entry));
    return createRelative(parent, name, access, row.disposition, row.options);
}

std::expected<UniqueHandle, DWORD> reopenHeld(HANDLE held, DWORD access)
{
    return createRelative(held, std::wstring_view {}, access, FILE_OPEN, ReopenOptions);
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
