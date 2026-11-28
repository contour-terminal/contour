// SPDX-License-Identifier: Apache-2.0
//
// The component walker of Windows: each directory is opened beneath the one before it with
// NtCreateFile, RootDirectory set to the held handle, as the reparse point itself and with
// OBJ_DONT_REPARSE, and held by its handle, so the walk never names a directory by a path again.
// A name surrogate met on the way is a link: a relative symbolic link is read with
// FSCTL_GET_REPARSE_POINT and spliced by the shared walk, which also owns the hop limit and `..` at
// the root; anything else that names another object is NotPermitted.
#include <core/fs/windows/WindowsPathWalk.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/fs/windows/NtFile.hpp>
#include <core/fs/windows/ReparsePoint.hpp>
#include <core/platform/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace core::fs::detail
{

namespace
{

    std::uint64_t littleEndianAt(std::span<BYTE const> bytes, std::size_t offset) noexcept
    {
        auto value = std::uint64_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 8 }))
            value |= std::uint64_t { bytes[offset + index] } << (index * 8U);
        return value;
    }

    class SystemNtFiles final: public INtFiles
    {
      public:
        FsResult<UniqueHandle> open(HANDLE directory,
                                    std::wstring const& name,
                                    NtOpenRequest const& request) const override
        {
            return ntOpenRelative(directory, name, request);
        }

        FsResult<EntryAttributes> attributes(HANDLE handle) const override
        {
            auto tag = FILE_ATTRIBUTE_TAG_INFO {};
            if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE)
                return EntryAttributes { .attributes = tag.FileAttributes, .reparseTag = tag.ReparseTag };
            // FAT and exFAT keep no reparse point and answer no FileAttributeTagInfo: their
            // attributes are the basic ones, and no tag.
            auto basic = FILE_BASIC_INFO {};
            if (::GetLastError() != ERROR_INVALID_PARAMETER
                || ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE)
                return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() },
                                                               "GetFileInformationByHandleEx") };
            return EntryAttributes { .attributes = basic.FileAttributes, .reparseTag = 0 };
        }

        /// The 128-bit `FILE_ID_INFO` where the file system has it, the 64-bit index of
        /// `BY_HANDLE_FILE_INFORMATION` where it does not (FAT).
        FsResult<FileIdentity> identity(HANDLE handle) const override
        {
            auto id = FILE_ID_INFO {};
            if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) != FALSE)
            {
                auto const bytes = std::span<BYTE const> { id.FileId.Identifier };
                return FileIdentity { .volume = id.VolumeSerialNumber,
                                      .idLow = littleEndianAt(bytes, 0),
                                      .idHigh = littleEndianAt(bytes, 8) };
            }
            auto legacy = BY_HANDLE_FILE_INFORMATION {};
            if (::GetFileInformationByHandle(handle, &legacy) == FALSE)
                return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() },
                                                               "GetFileInformationByHandle") };
            return FileIdentity { .volume = legacy.dwVolumeSerialNumber,
                                  .idLow =
                                      (std::uint64_t { legacy.nFileIndexHigh } << 32U) | legacy.nFileIndexLow,
                                  .idHigh = 0 };
        }

        FsResult<void> queryDirectory(HANDLE directory, std::span<std::byte> records) const override
        {
            if (::GetFileInformationByHandleEx(
                    directory, FileIdBothDirectoryInfo, records.data(), static_cast<DWORD>(records.size()))
                == FALSE)
                return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() },
                                                               "GetFileInformationByHandleEx") };
            return {};
        }
    };

    std::unexpected<FsError> refuseWith(FsErrorCode code)
    {
        return std::unexpected { FsError { .code = code, .systemCode = 0, .context = {} } };
    }

    /// What every name the walk looks at is opened with: enough to read its attributes and its
    /// reparse data, and nothing that a share mode or a missing right could refuse.
    constexpr auto LookRequest = NtOpenRequest { .access = FILE_READ_ATTRIBUTES,
                                                 .disposition = FILE_OPEN,
                                                 .options = 0,
                                                 .reparse = ReparseHandling::OpenThePoint };

} // namespace

INtFiles const& systemNtFiles() noexcept
{
    static auto const files = SystemNtFiles {};
    return files;
}

FsResult<OpenedEntry> openEntry(INtFiles const& files,
                                HANDLE directory,
                                std::wstring const& name,
                                NtOpenRequest request)
{
    request.access |= FILE_READ_ATTRIBUTES;
    request.reparse = ReparseHandling::OpenThePoint;
    auto point = files.open(directory, name, request);
    if (!point)
        return std::unexpected { point.error() };
    auto const attributes = files.attributes(point->get());
    if (!attributes)
        return std::unexpected { attributes.error() };
    if (!attributes->isFilteredPoint())
        return OpenedEntry { .handle = std::move(*point), .attributes = *attributes };

    auto entry = OpenedEntry { .handle = std::move(*point), .attributes = *attributes };
    auto filtered = openAgain(files, entry, request);
    if (!filtered)
    {
        if (filtered.error().systemCode == std::int64_t { STATUS_IO_REPARSE_TAG_NOT_HANDLED })
            return entry;
        return std::unexpected { filtered.error() };
    }
    return OpenedEntry { .handle = std::move(*filtered), .attributes = entry.attributes };
}

FsResult<UniqueHandle> openAgain(INtFiles const& files, OpenedEntry const& entry, NtOpenRequest request)
{
    return openAgain(files, entry.handle.get(), entry.attributes, request);
}

FsResult<UniqueHandle> openAgain(INtFiles const& files,
                                 HANDLE handle,
                                 EntryAttributes const& attributes,
                                 NtOpenRequest request)
{
    // By the handle, so whatever the name names by now it is this object that is opened; never
    // created again, nor truncated by the disposition.
    request.access |= FILE_READ_ATTRIBUTES;
    request.disposition = FILE_OPEN;
    request.reparse =
        attributes.isFilteredPoint() ? ReparseHandling::ThroughFilters : ReparseHandling::OpenThePoint;
    auto again = files.open(handle, std::wstring {}, request);
    if (!again)
    {
        // The object became a point the object manager would follow (a mount point) since it was
        // checked: that is a link, never crossed.
        if (again.error().systemCode == std::int64_t { STATUS_REPARSE_POINT_ENCOUNTERED })
            return refuseWith(FsErrorCode::NotPermitted);
        return std::unexpected { again.error() };
    }
    auto const checked = files.identity(handle);
    auto const opened = files.identity(again->get());
    if (!checked)
        return std::unexpected { checked.error() };
    if (!opened)
        return std::unexpected { opened.error() };
    if (*checked != *opened)
        return refuseWith(FsErrorCode::NotPermitted);
    return std::move(*again);
}

WindowsCursor::WindowsCursor(HANDLE root, INtFiles const& files) noexcept: _root { root }, _files { &files }
{
}

bool WindowsCursor::atRoot() const noexcept
{
    return _entered.empty();
}

HANDLE WindowsCursor::current() const noexcept
{
    return _entered.empty() ? _root : _entered.back().get();
}

FsResult<EntryKind> WindowsCursor::enter(std::string const& name)
{
    auto const wide = utf16Of(name);
    if (!wide)
        return std::unexpected { wide.error() };
    auto opened = openEntry(*_files,
                            current(),
                            *wide,
                            NtOpenRequest { .access = FILE_READ_ATTRIBUTES,
                                            .disposition = FILE_OPEN,
                                            .options = FILE_DIRECTORY_FILE,
                                            .reparse = ReparseHandling::OpenThePoint });
    if (opened)
    {
        // A directory link or a junction carries the directory attribute and opens as one.
        if (opened->attributes.isLink())
            return EntryKind::SymbolicLink;
        _entered.push_back(std::move(opened->handle));
        return EntryKind::Directory;
    }
    // A link to a file has no directory attribute, so FILE_DIRECTORY_FILE refuses it as it refuses
    // a file; and a point OBJ_DONT_REPARSE met is Loop. Which it is, the name itself tells.
    auto const failure = opened.error();
    if (failure.code != FsErrorCode::NotDirectory && failure.code != FsErrorCode::Loop)
        return std::unexpected { failure };
    return inspect(name).and_then([&failure](EntryKind kind) -> FsResult<EntryKind> {
        switch (kind)
        {
            case EntryKind::Missing: return refuseWith(FsErrorCode::NoEntry);
            // A directory here changed under the walk since the open refused it: reported as a
            // link, whose read answers Invalid, so the walk looks at the name again.
            case EntryKind::Directory:
            case EntryKind::SymbolicLink: return EntryKind::SymbolicLink;
            case EntryKind::Other: break;
        }
        return std::unexpected { failure };
    });
}

void WindowsCursor::leave()
{
    _entered.pop_back();
}

FsResult<EntryKind> WindowsCursor::inspect(std::string const& name)
{
    auto const wide = utf16Of(name);
    if (!wide)
        return std::unexpected { wide.error() };
    auto const opened = _files->open(current(), *wide, LookRequest);
    if (!opened)
    {
        if (opened.error().code == FsErrorCode::NoEntry)
            return EntryKind::Missing;
        return std::unexpected { opened.error() };
    }
    return _files->attributes(opened->get()).transform([](EntryAttributes const& attributes) {
        if (attributes.isLink())
            return EntryKind::SymbolicLink;
        if (attributes.isDirectory())
            return EntryKind::Directory;
        return EntryKind::Other;
    });
}

FsResult<std::string> WindowsCursor::readLink(std::string const& name)
{
    return utf16Of(name)
        .and_then([this](std::wstring const& wide) { return _files->open(current(), wide, LookRequest); })
        .and_then([](UniqueHandle const& link) { return readReparseData(link.get()); })
        .and_then([](ReparseData const& data) { return followedTarget(data); });
}

FsResult<Shortcut> WindowsCursor::shortcut(std::span<std::string const>)
{
    return Shortcut::Declined;
}

void WindowsCursor::restart()
{
    _entered.clear();
}

UniqueHandle WindowsCursor::release() noexcept
{
    if (_entered.empty())
        return UniqueHandle { platform::InvalidHandle };
    auto top = std::move(_entered.back());
    _entered.clear();
    return top;
}

FsResult<Resolved> resolveBeneath(platform::NativeHandle root,
                                  PathPlan plan,
                                  WalkRules const& rules,
                                  IBeneathOpener const* /*opener: Windows has none*/)
{
    return resolveBeneathWith(systemNtFiles(), root, std::move(plan), rules);
}

FsResult<Resolved> resolveBeneathWith(INtFiles const& files,
                                      HANDLE root,
                                      PathPlan plan,
                                      WalkRules const& rules)
{
    auto cursor = WindowsCursor { root, files };
    return walkBeneath(cursor, std::move(plan), rules).transform([&cursor, root](WalkEnd end) {
        return Resolved { .held = cursor.release(),
                          .root = root,
                          .name = std::move(end.name),
                          .kind = end.kind,
                          .finalComponent = end.finalComponent };
    });
}

} // namespace core::fs::detail
