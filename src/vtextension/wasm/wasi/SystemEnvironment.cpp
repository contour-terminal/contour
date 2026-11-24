// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wasi/SystemEnvironment.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#ifdef _WIN32
// clang-format off
    #include <windows.h>
    #include <bcrypt.h>
// clang-format on
#elifdef __linux__
    #include <sys/random.h>

    #include <cerrno>
#else
    #include <stdlib.h> // arc4random_buf
#endif

namespace vtextension::wasm::wasi
{

namespace
{
    /// @return The time of @p clock since its epoch, in nanoseconds.
    template <typename Clock>
    [[nodiscard]] uint64_t nanosecondsOf() noexcept
    {
        auto const elapsed = Clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    /// @return The time of the clock @p clockId, ClockId::Realtime or ClockId::Monotonic, in nanoseconds.
    [[nodiscard]] uint64_t readClock(uint32_t clockId) noexcept
    {
        // The shim asks for no other clock (Preview1Shim.cpp, Clocks).
        if (clockId == static_cast<uint32_t>(ClockId::Realtime))
            return nanosecondsOf<std::chrono::system_clock>();
        return nanosecondsOf<std::chrono::steady_clock>();
    }

    /// Fills @p bytes from the system's cryptographic random source. A source that refuses is a process
    /// that forbids the call (a sandbox): a guest's random bytes are never quietly predictable, so that
    /// terminates.
    void fillRandom(std::span<std::byte> bytes) noexcept
    {
#ifdef _WIN32
        // A guest's request is at most its memory, 4 GiB: ULONG holds every length but that one.
        while (!bytes.empty())
        {
            auto const chunk = std::min<std::size_t>(bytes.size(), ULONG { 1 } << 30U);
            if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr,
                                                reinterpret_cast<PUCHAR>(bytes.data()),
                                                static_cast<ULONG>(chunk),
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
                std::terminate();
            bytes = bytes.subspan(chunk);
        }
#elifdef __linux__
        // getrandom fills at most 32 MiB per call, and a signal can interrupt it.
        while (!bytes.empty())
        {
            auto const filled = ::getrandom(bytes.data(), bytes.size(), 0);
            if (filled < 0)
            {
                if (errno == EINTR)
                    continue;
                std::terminate();
            }
            bytes = bytes.subspan(static_cast<std::size_t>(filled));
        }
#else
        ::arc4random_buf(bytes.data(), bytes.size());
#endif
    }
} // namespace

ShimEnvironment systemEnvironment()
{
    return ShimEnvironment { .clock = &readClock, .random = &fillRandom };
}

} // namespace vtextension::wasm::wasi
