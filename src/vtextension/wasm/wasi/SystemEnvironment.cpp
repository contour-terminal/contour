// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wasi/SystemEnvironment.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
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
    [[nodiscard]] uint64_t readClock(ClockId clockId) noexcept
    {
        // The shim asks for no other clock (Preview1Shim.cpp, Clocks).
        if (clockId == ClockId::Realtime)
            return nanosecondsOf<std::chrono::system_clock>();
        return nanosecondsOf<std::chrono::steady_clock>();
    }

    /// Fills @p bytes from the system's cryptographic random source.
    /// @return Nothing once filled; `Io` if the source refused (a sandbox forbidding the call). A guest's
    ///         random bytes are never quietly predictable: its random_get fails instead, and only its.
    [[nodiscard]] std::expected<void, Errno> fillRandom(std::span<std::byte> bytes) noexcept
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
                return std::unexpected(Errno::Io);
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
                return std::unexpected(Errno::Io);
            }
            bytes = bytes.subspan(static_cast<std::size_t>(filled));
        }
#else
        ::arc4random_buf(bytes.data(), bytes.size());
#endif
        return {};
    }
} // namespace

ShimEnvironment systemEnvironment()
{
    return ShimEnvironment { &readClock, &fillRandom };
}

} // namespace vtextension::wasm::wasi
