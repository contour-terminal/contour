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
#include <terminal/pty/Pty.h>

#if defined(__linux__)
    #include <terminal/pty/LinuxPty.h>
#elif defined(_MSC_VER)
    #include <terminal/pty/ConPty.h>
#else
    #include <terminal/pty/UnixPty.h>
#endif

using std::make_unique;
using std::optional;
using std::unique_ptr;

namespace terminal
{

unique_ptr<Pty> createPty(PageSize pageSize, optional<crispy::ImageSize> viewSize)
{
#if defined(__linux__)
    return make_unique<terminal::LinuxPty>(pageSize, viewSize);
#elif defined(_MSC_VER)
    return make_unique<terminal::ConPty>(pageSize /*TODO: , viewSize*/);
#else
    return make_unique<terminal::UnixPty>(pageSize, viewSize);
#endif
}

} // namespace terminal
