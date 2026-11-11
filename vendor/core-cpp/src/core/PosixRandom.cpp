// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>
#include <core/SystemRandomSource.hpp>

#include <algorithm>
#include <cerrno>

#include <unistd.h>

// Apple and illumos declare getentropy() in <sys/random.h>, glibc, musl and Emscripten in <unistd.h>.
#if __has_include(<sys/random.h>)
    #include <sys/random.h>
#endif

namespace core::detail
{

namespace
{
    /// The most getentropy() takes in one call (POSIX.1-2024; the same limit on every system that
    /// has it), and what it fills whole or not at all.
    constexpr auto MaxEntropyRequest = std::size_t { 256 };
} // namespace

void fillFromSystemRandom(std::span<std::byte> out)
{
    while (!out.empty())
    {
        auto const chunk = std::min(out.size(), MaxEntropyRequest);
        if (::getentropy(out.data(), chunk) != 0)
        {
            // Not every getentropy() retries an interrupted call itself.
            if (errno == EINTR)
                continue;
            core::detail::fail("getentropy()", "The system random source failed.", __FILE__, __LINE__);
        }
        out = out.subspan(chunk);
    }
}

} // namespace core::detail
