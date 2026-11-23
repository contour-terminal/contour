// SPDX-License-Identifier: Apache-2.0
#include <vtpty/Win32BeneathRead.hpp>
#include <vtpty/WindowsBeneathRules.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <utility>

namespace vtpty
{

// The rule spells the Win32 values as numbers so that it compiles everywhere; here they meet the SDK.
static_assert(WindowsAttribute::Directory == FILE_ATTRIBUTE_DIRECTORY);
static_assert(WindowsAttribute::ReparsePoint == FILE_ATTRIBUTE_REPARSE_POINT);
static_assert(WindowsAttribute::Offline == FILE_ATTRIBUTE_OFFLINE);
static_assert(WindowsAttribute::RecallOnOpen == FILE_ATTRIBUTE_RECALL_ON_OPEN);
static_assert(WindowsAttribute::RecallOnDataAccess == FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
static_assert(IsReparseTagNameSurrogate(WindowsNameSurrogateBit) == WindowsNameSurrogateBit);
static_assert(IsReparseTagNameSurrogate(~WindowsNameSurrogateBit) == 0);
static_assert(AcceptedStorageReparseTags.size() == 3);
static_assert(AcceptedStorageReparseTags[0].tag == IO_REPARSE_TAG_DEDUP
              && AcceptedStorageReparseTags[0].mask == 0);
static_assert(AcceptedStorageReparseTags[1].tag == IO_REPARSE_TAG_WOF
              && AcceptedStorageReparseTags[1].mask == 0);
static_assert(AcceptedStorageReparseTags[2].tag == IO_REPARSE_TAG_CLOUD
              && AcceptedStorageReparseTags[2].mask == IO_REPARSE_TAG_CLOUD_MASK);

namespace
{
    /// The access each BeneathEntry is opened with, at its enumerator's index.
    struct AccessRow
    {
        BeneathEntry entry;
        DWORD access;
    };

    /// One row per BeneathEntry, in its order. A read looks at an entry with attribute access only; its data
    /// is reached by reopenForRead(), and only once the entry is known to be a regular file.
    constexpr auto AccessRows = std::to_array<AccessRow>({
        { .entry = BeneathEntry::DirectoryCreatedIfMissing, .access = HeldDirectoryAccess },
        { .entry = BeneathEntry::Directory, .access = FILE_TRAVERSE | FILE_READ_ATTRIBUTES },
        { .entry = BeneathEntry::Entry, .access = FILE_READ_ATTRIBUTES },
    });
    static_assert(AccessRows.size() == std::to_underlying(BeneathEntry::Last) + std::size_t { 1 });
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, AccessRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(AccessRows.at(index).entry) == index;
                                      }),
                  "AccessRows is in BeneathEntry order");

    /// What a re-opened file is opened with: its data, and its attributes so that it can be described again.
    constexpr auto ReadAccess = DWORD { FILE_READ_DATA | FILE_READ_ATTRIBUTES };

    /// @return @p handle's @p Info, queried as @p infoClass; or the Win32 error.
    template <typename Info>
    [[nodiscard]] std::expected<Info, DWORD> fileInfo(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS infoClass)
    {
        auto info = Info {};
        if (GetFileInformationByHandleEx(handle, infoClass, &info, sizeof(info)) == FALSE)
            return std::unexpected(GetLastError());
        return info;
    }

    /// @return @p handle's identity: the volume serial and the 128-bit file id (FILE_ID_INFO); where the
    ///         volume has none (FAT, exFAT, some redirectors), the 32-bit serial and 64-bit index of
    ///         GetFileInformationByHandle. Or the Win32 error.
    [[nodiscard]] std::expected<FileIdentity, DWORD> identityOf(HANDLE handle)
    {
        auto const id = fileInfo<FILE_ID_INFO>(handle, FileIdInfo);
        if (id)
        {
            auto halves = std::array<uint64_t, 2> {};
            static_assert(sizeof(halves) == sizeof(id->FileId.Identifier));
            std::memcpy(halves.data(), id->FileId.Identifier, sizeof(halves));
            return FileIdentity { .device = id->VolumeSerialNumber,
                                  .inode = halves[0],
                                  .inodeHigh = halves[1] };
        }
        if (id.error() != ERROR_INVALID_PARAMETER && id.error() != ERROR_NOT_SUPPORTED)
            return std::unexpected(id.error());

        auto info = BY_HANDLE_FILE_INFORMATION {};
        if (GetFileInformationByHandle(handle, &info) == FALSE)
            return std::unexpected(GetLastError());
        return FileIdentity { .device = info.dwVolumeSerialNumber,
                              .inode = (uint64_t { info.nFileIndexHigh } << 32U) | info.nFileIndexLow,
                              .inodeHigh = 0 };
    }

    /// @return Which of WindowsHandleType @p handle is (GetFileType).
    [[nodiscard]] WindowsHandleType handleTypeOf(HANDLE handle) noexcept
    {
        return GetFileType(handle) == FILE_TYPE_DISK ? WindowsHandleType::Disk : WindowsHandleType::Other;
    }

    /// @return A FILETIME-valued LARGE_INTEGER as one unsigned count; 0 for a negative one.
    [[nodiscard]] uint64_t unsignedOf(LARGE_INTEGER value) noexcept
    {
        return value.QuadPart < 0 ? 0 : static_cast<uint64_t>(value.QuadPart);
    }

    /// This machine's calls.
    class NativeWin32BeneathSystem final: public IWin32BeneathSystem
    {
      public:
        [[nodiscard]] std::expected<UniqueHandle, DWORD> open(HANDLE parent,
                                                              std::wstring_view name,
                                                              BeneathEntry entry) override
        {
            return openBeneath(parent, name, entry, AccessRows.at(std::to_underlying(entry)).access);
        }

        [[nodiscard]] std::expected<UniqueHandle, DWORD> reopenForRead(HANDLE looked) override
        {
            return reopenHeld(looked, ReadAccess);
        }

        [[nodiscard]] std::expected<FileStatus, DWORD> status(HANDLE handle) override
        {
            auto const tags = fileInfo<FILE_ATTRIBUTE_TAG_INFO>(handle, FileAttributeTagInfo);
            if (!tags)
                return std::unexpected(tags.error());
            auto const standard = fileInfo<FILE_STANDARD_INFO>(handle, FileStandardInfo);
            if (!standard)
                return std::unexpected(standard.error());
            auto const basic = fileInfo<FILE_BASIC_INFO>(handle, FileBasicInfo);
            if (!basic)
                return std::unexpected(basic.error());
            auto const identity = identityOf(handle);
            if (!identity)
                return std::unexpected(identity.error());

            auto const isReparsePoint = (tags->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            auto const facts = WindowsEntryFacts { .attributes = tags->FileAttributes,
                                                   .reparseTag = isReparsePoint ? tags->ReparseTag : 0,
                                                   .handleType = handleTypeOf(handle) };
            // A delete-pending entry has no name left to reach it by, as an unlinked inode has none.
            auto const links =
                standard->DeletePending != FALSE ? uint64_t { 0 } : uint64_t { standard->NumberOfLinks };
            return FileStatus { .kind = windowsFileKindOf(facts),
                                .identity = *identity,
                                .links = links,
                                .sizeBytes = unsignedOf(standard->EndOfFile),
                                .modifiedUnixMs = unixMsOfFileTime(unsignedOf(basic->LastWriteTime)) };
        }

        [[nodiscard]] std::expected<std::size_t, DWORD> read(HANDLE handle, std::span<char> into) override
        {
            auto const asked = static_cast<DWORD>(std::min<std::size_t>(into.size(), MAXDWORD));
            auto got = DWORD { 0 };
            if (ReadFile(handle, into.data(), asked, &got, nullptr) == FALSE)
                return std::unexpected(GetLastError());
            return std::size_t { got };
        }
    };
} // namespace

IWin32BeneathSystem& nativeWin32BeneathSystem() noexcept
{
    static auto instance = NativeWin32BeneathSystem {};
    return instance;
}

} // namespace vtpty
