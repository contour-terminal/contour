// SPDX-License-Identifier: Apache-2.0
//
// The reparse data of symbolic links and mount points, field by field at the offsets of the driver
// kit's REPARSE_DATA_BUFFER: an 8-byte head (tag, length of what follows, reserved), then the
// offsets and lengths of the two names in bytes, a symbolic link's flags, and the names themselves,
// UTF-16, at those offsets from the start of the path buffer.
#include <core/fs/windows/ReparsePoint.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/windows/NtFile.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <winioctl.h>

namespace core::fs::detail
{

namespace
{

    /// Where each field is, in bytes from the start of the data.
    constexpr std::size_t TagOffset = 0;
    constexpr std::size_t DataLengthOffset = 4;
    constexpr std::size_t HeadSize = 8; ///< What `ReparseDataLength` does not count.
    constexpr std::size_t SubstituteOffsetOffset = 8;
    constexpr std::size_t SubstituteLengthOffset = 10;
    constexpr std::size_t PrintOffsetOffset = 12;
    constexpr std::size_t PrintLengthOffset = 14;
    constexpr std::size_t SymlinkFlagsOffset = 16;
    constexpr std::size_t SymlinkPathBufferOffset = 20;
    constexpr std::size_t MountPointPathBufferOffset = 16;

    /// The layout of a point that names another object.
    struct NamesLayout
    {
        std::uint32_t tag;
        std::size_t pathBuffer; ///< Where its names begin.
    };

    constexpr auto NamesLayouts = std::to_array<NamesLayout>({
        { .tag = IO_REPARSE_TAG_SYMLINK, .pathBuffer = SymlinkPathBufferOffset },
        { .tag = IO_REPARSE_TAG_MOUNT_POINT, .pathBuffer = MountPointPathBufferOffset },
    });

    std::unexpected<FsError> refuseWith(FsErrorCode code)
    {
        return std::unexpected { FsError { .code = code, .systemCode = 0, .context = {} } };
    }

    template <typename T>
    T readAt(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
        auto value = T {};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    template <typename T>
    void writeAt(std::vector<std::byte>& bytes, std::size_t offset, T value) noexcept
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    }

    /// Encodes a point of @p tag whose names begin at @p pathBuffer: the substitute name, then the
    /// print name, each followed by a NUL that the lengths do not count, as the system writes them.
    std::vector<std::byte> encodeNames(std::uint32_t tag,
                                       std::size_t pathBuffer,
                                       std::uint32_t flags,
                                       std::wstring_view substituteName,
                                       std::wstring_view printName)
    {
        auto const substituteBytes = substituteName.size() * sizeof(wchar_t);
        auto const printBytes = printName.size() * sizeof(wchar_t);
        auto bytes =
            std::vector<std::byte>(pathBuffer + substituteBytes + printBytes + (2 * sizeof(wchar_t)));
        writeAt(bytes, TagOffset, tag);
        writeAt(bytes, DataLengthOffset, static_cast<std::uint16_t>(bytes.size() - HeadSize));
        writeAt(bytes, SubstituteOffsetOffset, std::uint16_t { 0 });
        writeAt(bytes, SubstituteLengthOffset, static_cast<std::uint16_t>(substituteBytes));
        writeAt(bytes, PrintOffsetOffset, static_cast<std::uint16_t>(substituteBytes + sizeof(wchar_t)));
        writeAt(bytes, PrintLengthOffset, static_cast<std::uint16_t>(printBytes));
        if (pathBuffer == SymlinkPathBufferOffset)
            writeAt(bytes, SymlinkFlagsOffset, flags);
        std::memcpy(bytes.data() + pathBuffer, substituteName.data(), substituteBytes);
        std::memcpy(
            bytes.data() + pathBuffer + substituteBytes + sizeof(wchar_t), printName.data(), printBytes);
        return bytes;
    }

    /// The name of @p bytes at @p offset and @p length from @p pathBuffer; `Invalid` outside them.
    FsResult<std::wstring> nameAt(std::span<std::byte const> bytes,
                                  std::size_t pathBuffer,
                                  std::size_t offset,
                                  std::size_t length)
    {
        if (length % sizeof(wchar_t) != 0 || pathBuffer + offset + length > bytes.size())
            return refuseWith(FsErrorCode::Invalid);
        auto name = std::wstring(length / sizeof(wchar_t), L'\0');
        std::memcpy(name.data(), bytes.data() + pathBuffer + offset, length);
        return name;
    }

    /// What @c followedTarget and @c reportedTarget refuse: anything but a relative symbolic link.
    FsResult<void> requireRelativeLink(ReparseData const& data)
    {
        if (!isNameSurrogate(data.tag))
            return refuseWith(FsErrorCode::Invalid);
        if (data.tag != IO_REPARSE_TAG_SYMLINK || data.target != LinkTarget::Relative)
            return refuseWith(FsErrorCode::NotPermitted);
        return {};
    }

} // namespace

std::vector<std::byte> relativeSymlinkReparseData(std::wstring_view substituteName,
                                                  std::wstring_view printName)
{
    return encodeNames(
        IO_REPARSE_TAG_SYMLINK, SymlinkPathBufferOffset, SymlinkFlagRelative, substituteName, printName);
}

std::vector<std::byte> mountPointReparseData(std::wstring_view substituteName, std::wstring_view printName)
{
    return encodeNames(IO_REPARSE_TAG_MOUNT_POINT, MountPointPathBufferOffset, 0, substituteName, printName);
}

FsResult<ReparseData> parseReparseData(std::span<std::byte const> bytes)
{
    if (bytes.size() < HeadSize)
        return refuseWith(FsErrorCode::Invalid);
    auto data = ReparseData { .tag = readAt<std::uint32_t>(bytes, TagOffset),
                              .substituteName = {},
                              .printName = {},
                              .target = LinkTarget::Absolute };
    auto const length = HeadSize + readAt<std::uint16_t>(bytes, DataLengthOffset);
    if (length > bytes.size())
        return refuseWith(FsErrorCode::Invalid);
    auto const used = bytes.first(length);
    auto const layout = std::ranges::find(NamesLayouts, data.tag, &NamesLayout::tag);
    if (layout == NamesLayouts.end())
        return data;
    if (used.size() < layout->pathBuffer)
        return refuseWith(FsErrorCode::Invalid);
    auto substitute = nameAt(used,
                             layout->pathBuffer,
                             readAt<std::uint16_t>(used, SubstituteOffsetOffset),
                             readAt<std::uint16_t>(used, SubstituteLengthOffset));
    auto print = nameAt(used,
                        layout->pathBuffer,
                        readAt<std::uint16_t>(used, PrintOffsetOffset),
                        readAt<std::uint16_t>(used, PrintLengthOffset));
    if (!substitute)
        return std::unexpected { substitute.error() };
    if (!print)
        return std::unexpected { print.error() };
    data.substituteName = std::move(*substitute);
    data.printName = std::move(*print);
    if (layout->pathBuffer == SymlinkPathBufferOffset
        && (readAt<std::uint32_t>(used, SymlinkFlagsOffset) & SymlinkFlagRelative) != 0)
        data.target = LinkTarget::Relative;
    return data;
}

FsResult<std::string> followedTarget(ReparseData const& data)
{
    return requireRelativeLink(data).and_then([&] { return utf8Of(data.substituteName); });
}

FsResult<std::string> reportedTarget(ReparseData const& data)
{
    return requireRelativeLink(data).and_then([&] {
        auto spelled = data.printName;
        std::ranges::replace(spelled, L'/', L'\\');
        return utf8Of(!data.printName.empty() && spelled == data.substituteName ? data.printName
                                                                                : data.substituteName);
    });
}

FsResult<ReparseData> readReparseData(HANDLE handle)
{
    auto bytes = std::vector<std::byte>(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    auto returned = DWORD { 0 };
    if (::DeviceIoControl(handle,
                          FSCTL_GET_REPARSE_POINT,
                          nullptr,
                          0,
                          bytes.data(),
                          static_cast<DWORD>(bytes.size()),
                          &returned,
                          nullptr)
        == FALSE)
        return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() },
                                                       "DeviceIoControl(FSCTL_GET_REPARSE_POINT)") };
    return parseReparseData(std::span<std::byte const> { bytes }.first(returned));
}

FsResult<void> writeReparseData(HANDLE handle, std::span<std::byte const> data)
{
    if (data.size() > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
        return refuseWith(FsErrorCode::NameTooLong);
    auto returned = DWORD { 0 };
    if (::DeviceIoControl(handle,
                          FSCTL_SET_REPARSE_POINT,
                          const_cast<std::byte*>(data.data()),
                          static_cast<DWORD>(data.size()),
                          nullptr,
                          0,
                          &returned,
                          nullptr)
        == FALSE)
        return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() },
                                                       "DeviceIoControl(FSCTL_SET_REPARSE_POINT)") };
    return {};
}

} // namespace core::fs::detail
