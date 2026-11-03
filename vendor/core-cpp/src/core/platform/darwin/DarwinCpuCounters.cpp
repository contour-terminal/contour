// SPDX-License-Identifier: Apache-2.0
#include <core/platform/CpuCounters.hpp>
#include <core/platform/detail/CpuCounterProbe.hpp>
#include <core/platform/detail/UnavailableCpuCounterSource.hpp>

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include <libproc.h>
#include <unistd.h>

namespace core::platform
{

namespace
{
    /// Iterations of busy work that costs a few milliseconds of CPU time.
    constexpr std::uint64_t ProbeIterations = 2'000'000;

    /// The measures this backend reports; any other is not provided.
    constexpr auto Supported = std::array { CpuCounter::Instructions, CpuCounter::Cycles };

    /// The reason any other measure is not reported.
    constexpr std::string_view NotProvided = "not provided on macOS";

    /// Reads the process's resource usage, the counters among it.
    /// @param usage Receives the usage.
    /// @return 0 on success, otherwise the errno of the failed call.
    [[nodiscard]] int readUsage(rusage_info_v4& usage) noexcept
    {
        // proc_pid_rusage() takes the rusage_info_t out-parameter, a void* in disguise.
        if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) != 0)
            return errno;
        return 0;
    }

    /// Counts the instructions and cycles retired by this process, in user and kernel mode together.
    class DarwinCpuCounterSource final: public ICpuCounterSource
    {
      public:
        [[nodiscard]] CpuCounterSample read() override
        {
            auto usage = rusage_info_v4 {};
            auto sample = CpuCounterSample {};
            if (readUsage(usage) != 0)
                return sample;
            sample.set(CpuCounter::Instructions, usage.ri_instructions);
            sample.set(CpuCounter::Cycles, usage.ri_cycles);
            return sample;
        }

        [[nodiscard]] std::string_view unavailableReason(CpuCounter counter) const noexcept override
        {
            return std::ranges::find(Supported, counter) != std::ranges::end(Supported) ? std::string_view {}
                                                                                        : NotProvided;
        }

        [[nodiscard]] std::string_view note() const noexcept override { return {}; }
    };

    /// Runs busy work whose instructions the optimizer cannot remove.
    void burnCpu() noexcept
    {
        std::uint64_t volatile sink = 0;
        for (auto const index: std::views::iota(std::uint64_t { 0 }, ProbeIterations))
            sink = sink + index;
    }
} // namespace

std::unique_ptr<ICpuCounterSource> makeDefaultCpuCounterSource()
{
    auto before = rusage_info_v4 {};
    if (auto const error = readUsage(before); error != 0)
        return std::make_unique<detail::UnavailableCpuCounterSource>(std::generic_category().message(error));

    burnCpu();

    auto after = rusage_info_v4 {};
    if (auto const error = readUsage(after); error != 0)
        return std::make_unique<detail::UnavailableCpuCounterSource>(std::generic_category().message(error));

    if (detail::probeCounters(after.ri_instructions - before.ri_instructions)
        == detail::CounterProbe::NotPopulated)
        return std::make_unique<detail::UnavailableCpuCounterSource>("counters not populated (virtualized)");

    return std::make_unique<DarwinCpuCounterSource>();
}

} // namespace core::platform
