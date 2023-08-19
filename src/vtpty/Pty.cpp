// SPDX-License-Identifier: Apache-2.0
#include <vtpty/Pty.h>

#if defined(_MSC_VER)
    #include <vtpty/ConPty.h>
#else
    #include <vtpty/UnixPty.h>
#endif

using std::make_unique;
using std::optional;
using std::unique_ptr;

namespace terminal
{

unique_ptr<Pty> createPty(PageSize pageSize, optional<crispy::image_size> viewSize)
{
#if defined(_MSC_VER)
    return make_unique<terminal::ConPty>(pageSize /*TODO: , viewSize*/);
#else
    return make_unique<terminal::UnixPty>(pageSize, viewSize);
#endif
}

} // namespace terminal
