// SPDX-License-Identifier: Apache-2.0
#include <vtpty/OwnedFd.hpp>

#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace vtpty
{

void OwnedFd::reset() noexcept
{
    if (_fd < 0)
        return;
#ifdef _WIN32
    ::_close(_fd);
#else
    ::close(_fd);
#endif
    _fd = -1;
}

} // namespace vtpty
