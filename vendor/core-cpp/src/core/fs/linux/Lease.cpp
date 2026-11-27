// SPDX-License-Identifier: Apache-2.0
//
// Waiting for a lease on Linux. An O_NONBLOCK open of a regular file under a lease answers
// EWOULDBLOCK instead of breaking the lease; the open is made again, waiting, but never by the name:
// the name is opened as a path handle (O_PATH opens nothing and breaks no lease), checked by that
// handle to be a regular file, and reopened through /proc/self/fd, which names that very inode. A
// FIFO renamed over the name in between is seen by the check and never opened blocking.
#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <sys/stat.h>

#include <cerrno>
#include <expected>
#include <string>

#include <fcntl.h>

namespace core::fs::detail
{

FsResult<UniqueHandle> openAfterWouldBlock(int directory, std::string const& name, int flags)
{
    auto const wouldBlock = [](char const* why) {
        return std::unexpected { FsError {
            .code = FsErrorCode::WouldBlock, .systemCode = EWOULDBLOCK, .context = why } };
    };
    auto const path = UniqueHandle { retryOnInterrupt(
        [&] { return ::openat(directory, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC); }) };
    if (path.get() < 0)
        return errnoError("openat(O_PATH)");
    struct ::stat status {};
    if (::fstat(path.get(), &status) != 0)
        return errnoError("fstat");
    if (!S_ISREG(status.st_mode))
        return wouldBlock("the name no longer names a regular file");
    // The magic link resolves to the inode itself, so O_NOFOLLOW (which would refuse the link) and
    // the creating flags (it exists) are dropped.
    auto const reopen = "/proc/self/fd/" + std::to_string(path.get());
    auto const opened = retryOnInterrupt(
        [&] { return ::open(reopen.c_str(), flags & ~(O_NOFOLLOW | O_CREAT | O_EXCL | O_NONBLOCK)); });
    if (opened < 0)
    {
        if (errno == ENOENT)
            return wouldBlock("no /proc to wait for the lease through");
        return errnoError("open(/proc/self/fd)");
    }
    return UniqueHandle { opened };
}

} // namespace core::fs::detail
