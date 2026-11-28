// SPDX-License-Identifier: Apache-2.0
//
// The POSIX handles of StandardStreamsTestSupport.hpp.
#include <core/net/StandardStreamsTestSupport.hpp>

#include <array>
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace core::net::streams_test
{

namespace
{
    /// @return @p fd owned, or a throw naming @p what where it is -1.
    [[nodiscard]] OwnedHandle ownedOrThrow(int fd, char const* what)
    {
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), what);
        return OwnedHandle { fd };
    }
} // namespace

TestPipe makeTestPipe()
{
    auto fds = std::array<int, 2> {};
    if (::pipe(fds.data()) != 0)
        throw std::system_error(errno, std::generic_category(), "pipe");
    auto pipe = TestPipe { .read = OwnedHandle { fds[0] }, .write = OwnedHandle { fds[1] } };
    for (auto const fd: fds)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return pipe;
}

OwnedHandle openForReading(std::filesystem::path const& path)
{
    return ownedOrThrow(::open(path.c_str(), O_RDONLY | O_CLOEXEC), "open for reading");
}

OwnedHandle openForWriting(std::filesystem::path const& path)
{
    return ownedOrThrow(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600),
                        "open for writing");
}

OwnedHandle openNullDevice()
{
    return ownedOrThrow(::open("/dev/null", O_RDONLY | O_CLOEXEC), "open /dev/null");
}

StreamKind pipeStreamKind() noexcept
{
    return StreamKind::Pollable;
}

RunningReadStop runningReadStop() noexcept
{
    return RunningReadStop::Waits;
}

} // namespace core::net::streams_test
