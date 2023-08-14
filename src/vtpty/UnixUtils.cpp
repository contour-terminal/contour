/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
