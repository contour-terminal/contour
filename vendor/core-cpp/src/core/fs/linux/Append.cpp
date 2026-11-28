// SPDX-License-Identifier: Apache-2.0
//
// Appending on Linux: `pwritev2` with `RWF_APPEND` (Linux 4.16) appends through the descriptor's own
// handle, which stays without `O_APPEND`, so its positioned writes still land where they are aimed.
// Where the kernel or a seccomp filter refuses the flag, the append writes at the end `fstat`
// reports, through the same handle, under the descriptor's append lock: no reopen, which would
// need write permission granted again (a 0444 file, a chmod after open) and would drop the
// handle's O_SYNC/O_DSYNC.
#include <core/fs/linux/Append.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <span>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs::detail
{

namespace
{

    /// The errnos that say `RWF_APPEND` itself is what was refused.
    constexpr auto FlagRefusedErrnos = std::to_array({ EOPNOTSUPP, EINVAL, ENOSYS, EPERM });

    bool flagRefused(int error) noexcept
    {
        return std::ranges::find(FlagRefusedErrnos, error) != FlagRefusedErrnos.end();
    }

    /// Writes @p bytes through @p handle, one call at a time, until all are written or one fails.
    template <typename Write>
    FsResult<std::size_t> writeAll(std::span<std::byte const> bytes, Write write, char const* call)
    {
        auto written = std::size_t { 0 };
        while (written < bytes.size())
        {
            auto const count = retryOnInterrupt([&] { return write(bytes.subspan(written)); });
            if (count < 0)
            {
                if (written > 0)
                    break;
                return errnoError(call);
            }
            if (count == 0)
                break;
            written += static_cast<std::size_t>(count);
        }
        return written;
    }

} // namespace

FsResult<UniqueHandle> openAppendHandle(int, std::string const&, int, int)
{
    return UniqueHandle { platform::InvalidHandle };
}

FsResult<std::size_t> appendAtEnd(int handle, std::mutex& appendLock, std::span<std::byte const> bytes)
{
    auto const lock = std::scoped_lock { appendLock };
    struct ::stat status {};
    if (::fstat(handle, &status) != 0)
        return errnoError("fstat");
    auto const end = static_cast<std::uint64_t>(status.st_size);
    if (bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - end)
        return std::unexpected { FsError {
            .code = FsErrorCode::FileTooLarge, .systemCode = 0, .context = {} } };
    return writeAll(
        bytes,
        [&](std::span<std::byte const> rest) {
            auto const done = static_cast<std::uint64_t>(rest.data() - bytes.data());
            return ::pwrite(handle, rest.data(), rest.size(), static_cast<off_t>(end + done));
        },
        "pwrite(end)");
}

FsResult<std::size_t> appendBytes(int handle,
                                  OwnedHandle const*,
                                  std::mutex& appendLock,
                                  std::span<std::byte const> bytes)
{
    auto const appended = writeAll(
        bytes,
        [&](std::span<std::byte const> rest) {
            auto vector = ::iovec { .iov_base = const_cast<std::byte*>(rest.data()), .iov_len = rest.size() };
            // With RWF_APPEND the offset does not place the write; 0 leaves the handle's own offset alone.
            return ::pwritev2(handle, &vector, 1, 0, RWF_APPEND);
        },
        "pwritev2(RWF_APPEND)");
    if (!appended && flagRefused(static_cast<int>(appended.error().systemCode)))
        return appendAtEnd(handle, appendLock, bytes);
    return appended;
}

} // namespace core::fs::detail
