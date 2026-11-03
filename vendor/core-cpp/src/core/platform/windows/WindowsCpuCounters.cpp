// SPDX-License-Identifier: Apache-2.0
#include <core/platform/CpuCounters.hpp>
#include <core/platform/detail/UnavailableCpuCounterSource.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>

#include <windows.h>

namespace core::platform
{

namespace
{
    /// The measures this backend reports; any other is not provided.
    constexpr auto Supported = std::array { CpuCounter::ReferenceCycles };

    /// The reason any other measure is not reported.
    constexpr std::string_view NoRetiredCounters = "not available to unprivileged processes on Windows";

    /// Reads the cycles this process's threads have consumed, as ticks of a constant-rate clock (not core
    /// cycles), in user and kernel mode together.
    /// @param cycles Receives the count.
    /// @return 0 on success, otherwise the GetLastError() of the failed call.
    [[nodiscard]] DWORD readProcessCycles(std::uint64_t& cycles) noexcept
    {
        auto value = ULONG64 { 0 };
        if (QueryProcessCycleTime(GetCurrentProcess(), &value) == 0)
            return GetLastError();
        cycles = value;
        return 0;
    }

    /// Counts the constant-rate ticks of CPU time of every thread of this process.
    class WindowsCpuCounterSource final: public ICpuCounterSource
    {
      public:
        [[nodiscard]] CpuCounterSample read() override
        {
            auto sample = CpuCounterSample {};
            auto cycles = std::uint64_t { 0 };
            if (readProcessCycles(cycles) == 0)
                sample.set(CpuCounter::ReferenceCycles, cycles);
            return sample;
        }

        [[nodiscard]] std::string_view unavailableReason(CpuCounter counter) const noexcept override
        {
            return std::ranges::find(Supported, counter) != std::ranges::end(Supported) ? std::string_view {}
                                                                                        : NoRetiredCounters;
        }

        [[nodiscard]] std::string_view note() const noexcept override { return {}; }
    };
} // namespace

std::unique_ptr<ICpuCounterSource> makeDefaultCpuCounterSource()
{
    auto cycles = std::uint64_t { 0 };
    if (auto const error = readProcessCycles(cycles); error != 0)
        return std::make_unique<detail::UnavailableCpuCounterSource>(
            std::system_category().message(static_cast<int>(error)));
    return std::make_unique<WindowsCpuCounterSource>();
}

} // namespace core::platform
