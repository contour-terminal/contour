// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyBoundedRead.hpp>
#include <vtbackend/graphics/KittyMediumSource.hpp>
#include <vtbackend/graphics/KittyWin32Paths.hpp>

#include <vtpty/Win32Handle.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

// AclAPI.h and winternl.h use the types Windows.h declares and include nothing themselves, so it has to come
// first, against the alphabetical order.
// clang-format off
#include <Windows.h>
#include <AclAPI.h>
#include <winternl.h>
// clang-format on

namespace vtbackend::kitty_graphics
{

namespace
{
    using vtpty::UniqueHandle;

    static_assert(win32::attribute::Hidden == FILE_ATTRIBUTE_HIDDEN);
    static_assert(win32::attribute::System == FILE_ATTRIBUTE_SYSTEM);
    static_assert(win32::attribute::Directory == FILE_ATTRIBUTE_DIRECTORY);
    static_assert(win32::attribute::Device == FILE_ATTRIBUTE_DEVICE);
    static_assert(win32::attribute::ReparsePoint == FILE_ATTRIBUTE_REPARSE_POINT);
    static_assert(win32::attribute::Offline == FILE_ATTRIBUTE_OFFLINE);
    static_assert(win32::attribute::RecallOnOpen == FILE_ATTRIBUTE_RECALL_ON_OPEN);
    static_assert(win32::attribute::RecallOnDataAccess == FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
    static_assert(win32::drive_type::Fixed == DRIVE_FIXED);
    static_assert(win32::drive_type::RamDisk == DRIVE_RAMDISK);

    using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE,
                                            ACCESS_MASK,
                                            POBJECT_ATTRIBUTES,
                                            PIO_STATUS_BLOCK,
                                            PLARGE_INTEGER,
                                            ULONG,
                                            ULONG,
                                            ULONG,
                                            ULONG,
                                            PVOID,
                                            ULONG);

    /// What NtCreateFile is asked for one entry below the root.
    struct EntryOpen
    {
        ACCESS_MASK access;
        ULONG shareAccess;
        ULONG options;
    };

    /// A directory on the way: traversed and judged, never renamed or deleted while the walk holds it (no
    /// FILE_SHARE_DELETE), and never followed (FILE_OPEN_REPARSE_POINT).
    constexpr auto DirectoryOpen =
        EntryOpen { .access = FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    .shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE,
                    .options = FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT };

    /// The file read: nobody may write, rename or delete it while it is read (FILE_SHARE_READ only), nor is
    /// it fetched from elsewhere (FILE_OPEN_NO_RECALL).
    constexpr auto FileOpen =
        EntryOpen { .access = FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
                    .shareAccess = FILE_SHARE_READ,
                    .options = FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT
                               | FILE_SYNCHRONOUS_IO_NONALERT | FILE_SEQUENTIAL_ONLY | FILE_OPEN_NO_RECALL };

    /// Just enough to tell why an entry would not open as asked: its attributes.
    constexpr auto ProbeOpen =
        EntryOpen { .access = FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    .shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    .options = FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT };

    /// STATUS_ACCESS_DENIED: what opening the file with DELETE answers where its directory forbids deleting.
    constexpr auto StatusAccessDenied = static_cast<NTSTATUS>(0xC000'0022);

    /// Opens @p name below the directory @p parent, as one component and without following it.
    /// @return The handle; the NTSTATUS otherwise.
    [[nodiscard]] std::expected<UniqueHandle, NTSTATUS> openBelow(HANDLE parent,
                                                                  std::wstring const& name,
                                                                  EntryOpen const& open)
    {
        static auto* const ntCreateFile = vtpty::ntdllFunction<NtCreateFileFn>("NtCreateFile");
        if (ntCreateFile == nullptr)
            return std::unexpected(StatusAccessDenied);
        auto const bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
        auto objectName = UNICODE_STRING { .Length = bytes,
                                           .MaximumLength = bytes,
                                           .Buffer = const_cast<PWSTR>(name.c_str()) };
        auto attributes = OBJECT_ATTRIBUTES { .Length = sizeof(OBJECT_ATTRIBUTES),
                                              .RootDirectory = parent,
                                              .ObjectName = &objectName,
                                              .Attributes = OBJ_CASE_INSENSITIVE,
                                              .SecurityDescriptor = nullptr,
                                              .SecurityQualityOfService = nullptr };
        auto status = IO_STATUS_BLOCK {};
        auto handle = HANDLE { nullptr };
        auto const result = ntCreateFile(&handle,
                                         open.access,
                                         &attributes,
                                         &status,
                                         nullptr,
                                         0,
                                         open.shareAccess,
                                         FILE_OPEN,
                                         open.options,
                                         nullptr,
                                         0);
        if (result < 0)
            return std::unexpected(result);
        return UniqueHandle { handle };
    }

    /// @return The `FILE_ATTRIBUTE_*` bits of the opened @p handle; nullopt when they cannot be read.
    [[nodiscard]] std::optional<uint32_t> attributesOf(HANDLE handle) noexcept
    {
        auto info = FILE_ATTRIBUTE_TAG_INFO {};
        if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) == FALSE)
            return std::nullopt;
        return static_cast<uint32_t>(info.FileAttributes);
    }

    /// Judges the opened @p handle as @p role by its attributes.
    [[nodiscard]] std::expected<void, MediaRefusal> checkOpened(HANDLE handle, win32::EntryRole role)
    {
        auto const attributes = attributesOf(handle);
        if (!attributes)
            return std::unexpected(MediaRefusal::Unreadable);
        return win32::checkAttributes(*attributes, role);
    }

    /// Judges what opening @p name below @p parent as @p role gave. One that did not open as asked is looked
    /// at once more, by its attributes alone, to refuse a link as a link rather than as unreadable.
    [[nodiscard]] std::expected<UniqueHandle, MediaRefusal> judgedEntry(
        HANDLE parent,
        std::wstring const& name,
        std::expected<UniqueHandle, NTSTATUS> handle,
        win32::EntryRole role)
    {
        if (!handle)
        {
            if (auto const probe = openBelow(parent, name, ProbeOpen); probe)
                if (auto const judged = checkOpened(probe->get(), role); !judged)
                    return std::unexpected(judged.error());
            return std::unexpected(MediaRefusal::Unreadable);
        }
        if (auto const judged = checkOpened(handle->get(), role); !judged)
            return std::unexpected(judged.error());
        return std::move(*handle);
    }

    /// @return The final path of @p handle as a drive path (win32::drivePathOfFinalPath()).
    [[nodiscard]] std::expected<std::filesystem::path, MediaRefusal> finalPathOf(HANDLE handle)
    {
        auto buffer = std::wstring(MAX_PATH, L'\0');
        while (true)
        {
            auto const length = GetFinalPathNameByHandleW(handle,
                                                          buffer.data(),
                                                          static_cast<DWORD>(buffer.size()),
                                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (length == 0)
                return std::unexpected(MediaRefusal::Unreadable);
            if (length < buffer.size())
            {
                buffer.resize(length);
                break;
            }
            buffer.resize(length); // It answered the size it needs, its terminating NUL included.
        }
        return win32::drivePathOfFinalPath(buffer).transform(
            [](std::wstring path) { return std::filesystem::path { std::move(path) }; });
    }

    /// @return The root of the drive @p path lies on (`X:\`), as GetDriveTypeW asks it.
    [[nodiscard]] std::wstring driveRootOf(std::filesystem::path const& path)
    {
        return path.root_name().native() + L'\\';
    }

    /// Refuses @p path unless it lies on a fixed or RAM disk.
    [[nodiscard]] std::expected<void, MediaRefusal> checkDriveOf(std::filesystem::path const& path)
    {
        return win32::checkDriveType(static_cast<uint32_t>(GetDriveTypeW(driveRootOf(path).c_str())));
    }

    /// @return Whether @p lhs and @p rhs are the same path, as NTFS compares names: ignoring case.
    [[nodiscard]] bool samePath(std::filesystem::path const& lhs, std::filesystem::path const& rhs) noexcept
    {
        return CompareStringOrdinal(lhs.c_str(),
                                    static_cast<int>(lhs.native().size()),
                                    rhs.c_str(),
                                    static_cast<int>(rhs.native().size()),
                                    TRUE)
               == CSTR_EQUAL;
    }

    /// @return The bytes of the SID @p sid.
    [[nodiscard]] std::span<std::byte const> bytesOf(PSID sid) noexcept
    {
        return { static_cast<std::byte const*>(sid), static_cast<size_t>(GetLengthSid(sid)) };
    }

    /// @return Who owns the opened @p handle, compared with the @p readers.
    [[nodiscard]] std::expected<FileOwner, MediaRefusal> ownerOf(
        HANDLE handle, std::span<Win32MediumSource::Sid const> readers)
    {
        auto owner = PSID { nullptr };
        auto descriptor = PSECURITY_DESCRIPTOR { nullptr };
        if (GetSecurityInfo(handle,
                            SE_FILE_OBJECT,
                            OWNER_SECURITY_INFORMATION,
                            &owner,
                            nullptr,
                            nullptr,
                            nullptr,
                            &descriptor)
            != ERROR_SUCCESS)
            return std::unexpected(MediaRefusal::Unreadable);
        auto const isReader = owner != nullptr && IsValidSid(owner) != FALSE
                              && std::ranges::any_of(readers, [owner](Win32MediumSource::Sid const& reader) {
                                     return std::ranges::equal(reader, bytesOf(owner));
                                 });
        LocalFree(descriptor);
        return isReader ? FileOwner::Reader : FileOwner::Other;
    }

    /// @return What the policy judges of the opened file @p handle, or why it cannot tell.
    [[nodiscard]] std::expected<OpenedFile, MediaRefusal> openedFileOf(
        HANDLE handle, std::span<Win32MediumSource::Sid const> readers)
    {
        if (GetFileType(handle) != FILE_TYPE_DISK)
            return OpenedFile {
                .kind = FileKind::Other, .owner = FileOwner::Other, .linkCount = 0, .size = 0
            };
        auto standard = FILE_STANDARD_INFO {};
        if (GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE
            || standard.DeletePending != FALSE)
            return std::unexpected(MediaRefusal::Unreadable);
        return ownerOf(handle, readers).transform([&](FileOwner owner) {
            return OpenedFile { .kind = standard.Directory != FALSE ? FileKind::Other : FileKind::Regular,
                                .owner = owner,
                                .linkCount = static_cast<uint64_t>(standard.NumberOfLinks),
                                .size = static_cast<uint64_t>(standard.EndOfFile.QuadPart) };
        });
    }

    /// Reads what fits into @p into from @p handle, as readBounded() asks.
    [[nodiscard]] std::expected<size_t, MediaRefusal> readSome(HANDLE handle, std::span<uint8_t> into)
    {
        auto count = DWORD { 0 };
        auto const chunk = static_cast<DWORD>(std::min<size_t>(into.size(), MAXDWORD));
        if (ReadFile(handle, into.data(), chunk, &count, nullptr) == FALSE)
            return std::unexpected(MediaRefusal::Unreadable);
        return static_cast<size_t>(count);
    }

    /// Deletes the file @p handle was opened on, through that very handle: whatever its name says by now, and
    /// at once rather than when the last handle closes where the filesystem allows (POSIX semantics).
    void deleteThrough(HANDLE handle) noexcept
    {
        auto posix = FILE_DISPOSITION_INFO_EX { .Flags = FILE_DISPOSITION_FLAG_DELETE
                                                         | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
                                                         | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE };
        if (SetFileInformationByHandle(handle, FileDispositionInfoEx, &posix, sizeof(posix)) != FALSE)
            return;
        auto classic = FILE_DISPOSITION_INFO { .DeleteFile = TRUE };
        (void) SetFileInformationByHandle(handle, FileDispositionInfo, &classic, sizeof(classic));
    }

    /// What walking a path down from its root opened: every directory on the way, held until the read is
    /// done so none of them is moved meanwhile, and the file.
    struct Walked
    {
        std::vector<UniqueHandle> directories;
        UniqueHandle file;
        std::filesystem::path path; ///< The root's real path and the components walked.
        DeletionAfterRead deletion = DeletionAfterRead::Keep;
    };

    /// Opens @p requested one component at a time down from its root with NtCreateFile relative to the
    /// directory above, following no reparse point below the root: a junction's or a link's target is never
    /// looked up, so no component reaches a share nor leaves the root. The root is configuration, and its own
    /// links are followed.
    [[nodiscard]] std::expected<Walked, MediaRefusal> walkBeneath(
        RequestedPath const& requested,
        Medium medium,
        MediaPolicy const& policy,
        MediumReadInterleavings const& interleavings)
    {
        auto const rootSpelling = std::wstring { L"\\\\?\\" } + requested.root.native();
        auto root = UniqueHandle { CreateFileW(rootSpelling.c_str(),
                                               FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES
                                                   | SYNCHRONIZE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               nullptr,
                                               OPEN_EXISTING,
                                               FILE_FLAG_BACKUP_SEMANTICS,
                                               nullptr) };
        if (!root.valid())
            return std::unexpected(MediaRefusal::Unreadable);
        auto realRoot = finalPathOf(root.get());
        if (!realRoot)
            return std::unexpected(realRoot.error());
        if (auto const drive = checkDriveOf(*realRoot); !drive)
            return std::unexpected(drive.error());

        // No link below the root is followed, so the real path is known once the root is open: judged before
        // anything below the root is opened.
        auto const& components = requested.belowRoot;
        auto real = *realRoot;
        for (auto const& component: components)
            real /= component;
        if (auto const accepted = checkPath(policy, real, medium); !accepted)
            return std::unexpected(accepted.error());

        // Decided before the open, which asks for DELETE only when the file is to be deleted.
        auto walked = Walked { .directories = {},
                               .file = {},
                               .path = std::move(*realRoot),
                               .deletion = deletionAfterRead(policy, real, medium) };
        walked.directories.push_back(std::move(root));
        for (auto const& component: components | std::views::take(components.size() - 1))
        {
            if (interleavings.opening)
                interleavings.opening(walked.path / component);
            auto const parent = walked.directories.back().get();
            auto next = judgedEntry(parent,
                                    component.native(),
                                    openBelow(parent, component.native(), DirectoryOpen),
                                    win32::EntryRole::Directory);
            if (!next)
                return std::unexpected(next.error());
            walked.directories.push_back(std::move(*next));
            walked.path /= component;
        }

        if (interleavings.opening)
            interleavings.opening(walked.path / components.back());
        auto const& name = components.back().native();
        auto const parent = walked.directories.back().get();
        auto opened = std::expected<UniqueHandle, NTSTATUS> { std::unexpected(StatusAccessDenied) };
        if (walked.deletion == DeletionAfterRead::Delete)
        {
            auto withDelete = FileOpen;
            withDelete.access |= DELETE;
            opened = openBelow(parent, name, withDelete);
            // A file its directory forbids deleting is still read, and kept, as POSIX keeps one it cannot
            // unlink.
            if (!opened && opened.error() == StatusAccessDenied)
                walked.deletion = DeletionAfterRead::Keep;
        }
        if (walked.deletion == DeletionAfterRead::Keep)
            opened = openBelow(parent, name, FileOpen);
        auto file = judgedEntry(parent, name, std::move(opened), win32::EntryRole::File);
        if (!file)
            return std::unexpected(file.error());
        walked.file = std::move(*file);
        walked.path /= components.back();
        return walked;
    }
} // namespace

Win32MediumSource::Win32MediumSource(std::vector<Sid> readers, MediumReadInterleavings interleavings):
    _readers { std::move(readers) }, _interleavings { std::move(interleavings) }
{
}

std::expected<std::vector<uint8_t>, MediaRefusal> Win32MediumSource::read(std::string_view path,
                                                                          Medium medium,
                                                                          MediaPolicy const& policy) const
{
    // Judged as requested, before the filesystem is touched at all: as the policy has it, then as Windows
    // reads a spelling, then by its drive.
    auto const requested = requestedPath(policy, medium, path);
    if (!requested)
        return std::unexpected(requested.error());
    if (auto const spelled = win32::checkPathSpelling(path); !spelled)
        return std::unexpected(spelled.error());
    if (auto const drive = checkDriveOf(requested->path); !drive)
        return std::unexpected(drive.error());

    if (_interleavings.pathAccepted)
        _interleavings.pathAccepted(requested->path);

    auto walked = walkBeneath(*requested, medium, policy, _interleavings);
    if (!walked)
        return std::unexpected(walked.error());
    auto const& file = walked->file;

    // Defence in depth: the handle must name what was walked, which also refuses a short (8.3) alias.
    auto const finalPath = finalPathOf(file.get());
    if (!finalPath || !samePath(*finalPath, walked->path))
        return std::unexpected(MediaRefusal::Unreadable);

    auto const opened = openedFileOf(file.get(), _readers);
    if (!opened)
        return std::unexpected(opened.error());
    if (auto const accepted = checkOpenedFile(policy, *opened); !accepted)
        return std::unexpected(accepted.error());

    auto bytes = readBounded(opened->size, policy.sizeLimit, [&file](std::span<uint8_t> into) {
        return readSome(file.get(), into);
    });
    if (!bytes)
        return bytes;

    if (walked->deletion == DeletionAfterRead::Delete)
    {
        if (_interleavings.beforeDelete)
            _interleavings.beforeDelete(walked->path);
        deleteThrough(file.get());
    }
    return bytes;
}

std::expected<std::vector<Win32MediumSource::Sid>, std::error_code> currentReaderSids()
{
    auto token = HANDLE { nullptr };
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        return std::unexpected(vtpty::lastError());
    auto const owned = UniqueHandle { token };

    // A token information class read into a buffer of the size the token answers.
    auto const query =
        [&](TOKEN_INFORMATION_CLASS what) -> std::expected<std::vector<std::byte>, std::error_code> {
        auto size = DWORD { 0 };
        (void) GetTokenInformation(owned.get(), what, nullptr, 0, &size);
        auto buffer = std::vector<std::byte>(size);
        if (size == 0 || GetTokenInformation(owned.get(), what, buffer.data(), size, &size) == FALSE)
            return std::unexpected(vtpty::lastError());
        return buffer;
    };
    auto const sidCopy = [](PSID sid) {
        auto const bytes = bytesOf(sid);
        return Win32MediumSource::Sid { bytes.begin(), bytes.end() };
    };

    // The user, and the owner it gives what it creates: an elevated administrator's files belong to
    // BUILTIN\Administrators, not to the user.
    auto const user = query(TokenUser);
    if (!user)
        return std::unexpected(user.error());
    auto const owner = query(TokenOwner);
    if (!owner)
        return std::unexpected(owner.error());
    auto sids = std::vector<Win32MediumSource::Sid> { sidCopy(
        reinterpret_cast<TOKEN_USER const*>(user->data())->User.Sid) };
    if (auto ownerSid = sidCopy(reinterpret_cast<TOKEN_OWNER const*>(owner->data())->Owner);
        !std::ranges::contains(sids, ownerSid))
        sids.push_back(std::move(ownerSid));
    return sids;
}

} // namespace vtbackend::kitty_graphics
