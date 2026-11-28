// SPDX-License-Identifier: Apache-2.0
//
// Waiting for a lease on every POSIX system but Linux: none of them has leases, so an O_NONBLOCK
// open that answered EWOULDBLOCK is answered WouldBlock, and never opened again blocking.
#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/posix/PosixSystem.hpp>

#include <cerrno>
#include <expected>
#include <string>

namespace core::fs::detail
{

FsResult<UniqueHandle> openAfterWouldBlock(int, std::string const&, int)
{
    return std::unexpected { FsError {
        .code = FsErrorCode::WouldBlock, .systemCode = EWOULDBLOCK, .context = "openat" } };
}

} // namespace core::fs::detail
