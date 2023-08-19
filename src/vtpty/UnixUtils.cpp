// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixUtils.h>

#include <cstring>
#include <stdexcept>
#include <string>

using namespace std::string_literals;

namespace terminal
{

UnixPipe::UnixPipe(unsigned flags): pfd { -1, -1 }
{
#if defined(__linux__)
    if (pipe2(pfd, static_cast<int>(flags)) < 0)
        throw std::runtime_error { "Failed to create PTY pipe. "s + strerror(errno) };
#else
    if (pipe(pfd) < 0)
        throw std::runtime_error { "Failed to create PTY pipe. "s + strerror(errno) };
    for (auto const fd: pfd)
        if (!detail::setFileFlags(fd, flags))
            break;
#endif
}

} // namespace terminal
