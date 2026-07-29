// SPDX-License-Identifier: Apache-2.0
#include <vtpty/Pty.h>

// _WIN32, not _MSC_VER: a MinGW/clang Windows build must also pick ConPty —
// UnixPty does not exist there.
#ifdef _WIN32
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
#ifdef _WIN32
    return make_unique<ConPty>(pageSize /*TODO: , viewSize*/);
#else
    return make_unique<UnixPty>(pageSize, viewSize);
#endif
}

} // namespace vtpty
