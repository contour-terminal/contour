// SPDX-License-Identifier: Apache-2.0
//
// Appending where no write flag appends through a handle opened without `O_APPEND` (Apple, the BSDs,
// any POSIX system but Linux): a second handle, opened `O_APPEND` from the parent directory the
// first was opened beneath, by the same name, and checked by `(st_dev, st_ino)` to be the same
// object. Never `dup`, which shares the first handle's status flags and offset. The first is a
// regular file, so the O_NONBLOCK that keeps this open from waiting on a swapped-in FIFO changes
// nothing about its writes.
#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <cstddef>
#include <expected>
#include <mutex>
#include <span>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs::detail
{

FsResult<UniqueHandle> openAppendHandle(int directory, std::string const& name, int first, int integrityFlags)
{
    auto const second = retryOnInterrupt([&] {
        return ::openat(directory,
                        name.c_str(),
                        O_WRONLY | O_APPEND | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC | O_NONBLOCK
                            | integrityFlags);
    });
    if (second < 0)
        return errnoError("openat(O_APPEND)");
    auto handle = UniqueHandle { second };
    struct ::stat firstStatus {};
    struct ::stat secondStatus {};
    if (::fstat(first, &firstStatus) != 0 || ::fstat(second, &secondStatus) != 0)
        return errnoError("fstat");
    if (firstStatus.st_dev != secondStatus.st_dev || firstStatus.st_ino != secondStatus.st_ino)
        return std::unexpected { FsError { .code = FsErrorCode::Busy,
                                           .systemCode = 0,
                                           .context = "the name was replaced while it was opened" } };
    return handle;
}

FsResult<std::size_t> appendBytes(int,
                                  OwnedHandle const* appendHandle,
                                  std::mutex&,
                                  std::span<std::byte const> bytes)
{
    if (appendHandle == nullptr)
        return std::unexpected { FsError {
            .code = FsErrorCode::BadDescriptor, .systemCode = 0, .context = "no append handle was opened" } };
    auto written = std::size_t { 0 };
    while (written < bytes.size())
    {
        auto const rest = bytes.subspan(written);
        auto const count =
            retryOnInterrupt([&] { return ::write(appendHandle->get(), rest.data(), rest.size()); });
        if (count < 0)
        {
            if (written > 0)
                break;
            return errnoError("write(O_APPEND)");
        }
        if (count == 0)
            break;
        written += static_cast<std::size_t>(count);
    }
    return written;
}

} // namespace core::fs::detail
