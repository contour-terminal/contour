// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixUtils.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <unistd.h>

using namespace std::string_literals;

namespace vtpty
{

UnixPipe::UnixPipe(unsigned flags): pfd { -1, -1 }
{
#ifdef __linux__
    if (pipe2(pfd.data(), static_cast<int>(flags)) < 0)
        throw std::runtime_error { "Failed to create PTY pipe. "s + std::generic_category().message(errno) };
#else
    if (pipe(pfd.data()) < 0)
        throw std::runtime_error { "Failed to create PTY pipe. "s + std::generic_category().message(errno) };
    for (auto const fd: pfd)
        if (!util::setFileFlags(fd, static_cast<int>(flags)))
            break;
#endif
}

} // namespace vtpty
