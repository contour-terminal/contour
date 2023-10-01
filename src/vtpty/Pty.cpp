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

namespace vtpty
{

unique_ptr<Pty> createPty(PageSize pageSize, optional<ImageSize> viewSize)
{
#if defined(_MSC_VER)
    return make_unique<ConPty>(pageSize /*TODO: , viewSize*/);
#else
    return make_unique<UnixPty>(pageSize, viewSize);
#endif
}

} // namespace vtpty
