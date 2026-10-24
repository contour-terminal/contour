// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/StartupTimeline.hpp>

#include <chrono>
#include <format>

namespace vthost::latency
{

namespace
{
    [[nodiscard]] constexpr size_t indexOf(StartupMark mark) noexcept
    {
        return static_cast<size_t>(mark);
    }
} // namespace

StartupTimeline::StartupTimeline(core::platform::IClock const& clock,
                                 core::platform::SteadyTimePoint processStart):
    _clock { clock }
{
    _stamps[indexOf(StartupMark::ProcessStart)] = processStart;
}

void StartupTimeline::mark(StartupMark mark)
{
    auto const now = _clock.now();
    auto const lock = std::scoped_lock { _mutex };
    auto& stamp = _stamps[indexOf(mark)];
    if (!stamp)
        stamp = now;
}

std::optional<core::platform::SteadyTimePoint> StartupTimeline::at(StartupMark mark) const
{
    auto const lock = std::scoped_lock { _mutex };
    return _stamps[indexOf(mark)];
}

std::string StartupTimeline::describe() const
{
    auto const lock = std::scoped_lock { _mutex };
    auto const start = *_stamps[indexOf(StartupMark::ProcessStart)];
    auto text = std::string {};
    for (auto const& row: StartupMarkTable)
        if (auto const& stamp = _stamps[indexOf(row.mark)]; stamp)
            text += std::format("{:<26}+{:.1f} ms\n",
                                row.name,
                                std::chrono::duration<double, std::milli> { *stamp - start }.count());
    return text;
}

} // namespace vthost::latency
