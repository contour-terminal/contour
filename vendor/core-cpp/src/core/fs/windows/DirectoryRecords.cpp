// SPDX-License-Identifier: Apache-2.0
#include <core/fs/windows/DirectoryRecords.hpp>

#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/windows/NtFile.hpp>
#include <core/fs/windows/WindowsPathWalk.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace core::fs::detail
{

namespace
{

    /// Where each field a stream reads is, in bytes from the start of its record.
    constexpr std::size_t NextOffset = offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset);
    constexpr std::size_t AttributesOffset = offsetof(FILE_ID_BOTH_DIR_INFO, FileAttributes);
    constexpr std::size_t NameLengthOffset = offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength);
    /// Holds the reparse tag where the attributes say the entry is a reparse point.
    constexpr std::size_t ReparseTagOffset = offsetof(FILE_ID_BOTH_DIR_INFO, EaSize);
    constexpr std::size_t NameOffset = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);

    std::unexpected<FsError> invalid(char const* what)
    {
        return std::unexpected { FsError { .code = FsErrorCode::Invalid, .systemCode = 0, .context = what } };
    }

    std::uint32_t readWord(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
        auto value = std::uint32_t {};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    DescriptorType typeOf(EntryAttributes const& attributes) noexcept
    {
        if (attributes.isLink())
            return DescriptorType::SymbolicLink;
        if (attributes.isDirectory())
            return DescriptorType::Directory;
        return DescriptorType::RegularFile;
    }

} // namespace

FsResult<std::vector<FsResult<DirectoryEntry>>> parseDirectoryRecords(std::span<std::byte const> records)
{
    auto entries = std::vector<FsResult<DirectoryEntry>> {};
    auto offset = std::size_t { 0 };
    while (true)
    {
        if (records.size() < NameOffset || offset > records.size() - NameOffset)
            return invalid("FILE_ID_BOTH_DIR_INFO beyond its buffer");
        auto const record = records.subspan(offset);
        auto const nameBytes = std::size_t { readWord(record, NameLengthOffset) };
        if (nameBytes % sizeof(wchar_t) != 0 || nameBytes > record.size() - NameOffset)
            return invalid("FILE_ID_BOTH_DIR_INFO name beyond its buffer");
        auto name = std::wstring(nameBytes / sizeof(wchar_t), L'\0');
        std::memcpy(name.data(), record.data() + NameOffset, nameBytes);
        // No file system hands out an empty name or one holding a separator; one that did would name
        // another entry, or a path.
        if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos)
            entries.emplace_back(std::unexpected { FsError {
                .code = FsErrorCode::Io, .systemCode = 0, .context = "FILE_ID_BOTH_DIR_INFO name" } });
        else if (name != L"." && name != L"..")
        {
            auto const attributes = EntryAttributes {
                .attributes = readWord(record, AttributesOffset),
                .reparseTag = readWord(record, ReparseTagOffset),
            };
            entries.emplace_back(utf8Of(name).transform([&](std::string narrow) {
                return DirectoryEntry { .type = typeOf(attributes), .name = std::move(narrow) };
            }));
        }
        auto const next = std::size_t { readWord(record, NextOffset) };
        if (next == 0)
            return entries;
        if (next < NameOffset)
            return invalid("FILE_ID_BOTH_DIR_INFO chain does not move forward");
        offset += next;
    }
}

} // namespace core::fs::detail
