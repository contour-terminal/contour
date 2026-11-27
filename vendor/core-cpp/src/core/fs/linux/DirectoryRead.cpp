// SPDX-License-Identifier: Apache-2.0
//
// A directory stream's reads on Linux: getdents64 into one fixed buffer per stream, each record read
// field by field within what the call returned. No DIR, so no readdir and its shared entry buffer.
#include <core/fs/linux/DirectoryRead.hpp>

#include <core/async/IExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/DirectoryStream.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/DirectoryRead.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>

namespace core::fs::detail
{

/// The stream's own handle on the directory, which keeps its position, and the buffer its records
/// are read into; a batch holds @c lock while it reads.
struct DirectoryReader
{
    SharedHandle handle;                ///< Opened for the stream alone.
    std::mutex lock;                    ///< Held by the batch that reads.
    std::vector<std::uint64_t> records; ///< @c DirectoryBufferBytes, 8-byte aligned.

    explicit DirectoryReader(SharedHandle directory):
        handle { std::move(directory) }, records(DirectoryBufferBytes / sizeof(std::uint64_t))
    {
    }
};

namespace
{

    /// Where each field of a `dirent64` is, in bytes from the start of its record.
    constexpr std::size_t LengthOffset = offsetof(struct ::dirent64, d_reclen);
    constexpr std::size_t TypeOffset = offsetof(struct ::dirent64, d_type);
    constexpr std::size_t NameOffset = offsetof(struct ::dirent64, d_name);

    std::unexpected<FsError> invalid()
    {
        return std::unexpected { FsError { .code = FsErrorCode::Invalid,
                                           .systemCode = 0,
                                           .context = "getdents64 record beyond its buffer" } };
    }

} // namespace

FsResult<void> parseDirentRecords(int directory, std::span<std::byte const> bytes, DirectoryBatch& batch)
{
    auto offset = std::size_t { 0 };
    while (offset < bytes.size())
    {
        auto const record = bytes.subspan(offset);
        if (record.size() <= NameOffset)
            return invalid();
        auto length = std::uint16_t {};
        std::memcpy(&length, record.data() + LengthOffset, sizeof(length));
        if (length <= NameOffset || length > record.size())
            return invalid();
        auto const name = record.subspan(NameOffset, length - NameOffset);
        auto const end = std::ranges::find(name, std::byte { 0 });
        if (end == name.end())
            return invalid();
        auto text = std::string(static_cast<std::size_t>(end - name.begin()), '\0');
        std::memcpy(text.data(), name.data(), text.size());
        if (auto entry = entryOf(directory, text.c_str(), std::to_integer<unsigned char>(record[TypeOffset])))
            batch.entries.push_back(std::move(*entry));
        offset += length;
    }
    return {};
}

FsResult<std::shared_ptr<DirectoryReader>> openDirectoryReader(int directory, async::IExecutor& closer)
{
    auto const opened =
        retryOnInterrupt([&] { return ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC); });
    if (opened < 0)
        return errnoError("openat");
    return std::make_shared<DirectoryReader>(shareHandle(UniqueHandle { opened }, closer));
}

FsResult<DirectoryBatch> readDirectoryBatch(DirectoryReader& reader)
{
    auto const held = std::scoped_lock { reader.lock };
    auto const buffer = std::as_writable_bytes(std::span { reader.records });
    auto const count =
        retryOnInterrupt([&] { return ::getdents64(reader.handle->get(), buffer.data(), buffer.size()); });
    auto batch = DirectoryBatch {};
    // A directory removed while it is read has nothing more (ENOENT), as readdir says too.
    if (count == 0 || (count < 0 && errno == ENOENT))
    {
        batch.end = StreamEnd::Reached;
        return batch;
    }
    if (count < 0)
        return errnoError("getdents64");
    return parseDirentRecords(reader.handle->get(), buffer.first(static_cast<std::size_t>(count)), batch)
        .transform([&] { return std::move(batch); });
}

} // namespace core::fs::detail
