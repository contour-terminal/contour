// SPDX-License-Identifier: Apache-2.0
//
// A directory stream's reads on every POSIX system but Linux: fdopendir over the stream's own
// handle, and readdir, whose entry buffer belongs to that DIR alone and is read under the stream's
// lock. Up to detail::DirectoryBatchEntries entries a batch.
#include <core/fs/posix/DirectoryRead.hpp>

#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/DirectoryStream.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <cerrno>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>

#include <dirent.h>
#include <fcntl.h>

namespace core::fs::detail
{

namespace
{

    struct DirectoryCloser
    {
        void operator()(DIR* directory) const noexcept
        {
            // Closes the handle fdopendir took over, too; not retried, as closeNativeHandle says.
            ::closedir(directory);
        }
    };

    using UniqueDirectory = std::unique_ptr<DIR, DirectoryCloser>;

    /// Closes @p directory on @p closer, or there and then should the executor free the frame
    /// without running it.
    async::DetachedTask closeDirectoryOn(async::IExecutor* closer, UniqueDirectory directory)
    {
        co_await async::ResumeOn { *closer };
        directory.reset();
    }

} // namespace

/// The stream's own DIR, closed by its last owner on the blocking executor as an @c OwnedHandle is;
/// a batch holds @c lock while it reads.
struct DirectoryReader
{
    UniqueDirectory directory;
    async::IExecutor* closer;
    std::mutex lock; ///< Held by the batch that reads.
    /// A readdir failure met after a batch's first entries, answered by the batch after it; under
    /// @c lock.
    std::optional<FsError> failure;

    DirectoryReader(UniqueDirectory opened, async::IExecutor& closeOn) noexcept:
        directory { std::move(opened) }, closer { &closeOn }, failure {}
    {
    }

    DirectoryReader(DirectoryReader const&) = delete;
    DirectoryReader(DirectoryReader&&) = delete;
    DirectoryReader& operator=(DirectoryReader const&) = delete;
    DirectoryReader& operator=(DirectoryReader&&) = delete;

    ~DirectoryReader()
    {
        try
        {
            closeDirectoryOn(closer, std::move(directory));
        }
        catch (std::bad_alloc const&)
        {
            directory.reset();
        }
    }
};

FsResult<std::shared_ptr<DirectoryReader>> openDirectoryReader(int directory, async::IExecutor& closer)
{
    auto const opened =
        retryOnInterrupt([&] { return ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC); });
    if (opened < 0)
        return errnoError("openat");
    auto handle = UniqueHandle { opened };
    auto* const stream = ::fdopendir(handle.get());
    if (stream == nullptr)
        return errnoError("fdopendir");
    // fdopendir owns the handle from here on; closedir closes it.
    std::ignore = handle.release();
    return std::make_shared<DirectoryReader>(UniqueDirectory { stream }, closer);
}

FsResult<DirectoryBatch> readDirectoryBatch(DirectoryReader& reader)
{
    auto const held = std::scoped_lock { reader.lock };
    if (reader.failure)
        return std::unexpected { *std::exchange(reader.failure, std::nullopt) };
    auto batch = DirectoryBatch {};
    auto const handle = ::dirfd(reader.directory.get());
    for ([[maybe_unused]] auto const read: std::views::iota(std::size_t { 0 }, DirectoryBatchEntries))
    {
        errno = 0;
        auto const* const entry = ::readdir(reader.directory.get());
        if (entry == nullptr)
        {
            if (errno == 0)
                batch.end = StreamEnd::Reached;
            else if (batch.entries.empty())
                return errnoError("readdir");
            else
                // The entries read come first; the next batch answers the failure.
                reader.failure = errnoError("readdir").error();
            return batch;
        }
        if (auto answer = entryOf(handle, entry->d_name, entry->d_type))
            batch.entries.push_back(std::move(*answer));
    }
    return batch;
}

} // namespace core::fs::detail
