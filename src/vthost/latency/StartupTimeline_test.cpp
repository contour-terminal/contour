// SPDX-License-Identifier: Apache-2.0

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <set>
#include <string_view>

#include <vthost/latency/StartupTimeline.hpp>

using namespace vthost::latency;

TEST_CASE("the timeline keeps the first stamp of each mark, in table order", "[vthost][latency]")
{
    using namespace std::chrono_literals;
    auto clock = core::platform::ManualClock { core::platform::SteadyTimePoint {} + 1s };
    auto timeline = StartupTimeline { clock, clock.now() };
    clock.advance(12ms);
    timeline.mark(StartupMark::Connected);
    clock.advance(3ms);
    timeline.mark(StartupMark::ConfigLoaded);
    clock.advance(100ms);
    timeline.mark(StartupMark::Connected); // ignored
    CHECK(timeline.at(StartupMark::Connected) == core::platform::SteadyTimePoint {} + 1012ms);
    CHECK_FALSE(timeline.at(StartupMark::WindowCreated).has_value());
    CHECK(timeline.describe()
          == std::format("{:<26}+0.0 ms\n{:<26}+15.0 ms\n{:<26}+12.0 ms\n",
                         "process-start",
                         "config-loaded",
                         "connected"));
}

TEST_CASE("every StartupMark has exactly one named table row", "[vthost][latency]")
{
    CHECK(StartupMarkTable.size() == static_cast<size_t>(StartupMark::FirstPromptSwapped) + 1);
    auto names = std::set<std::string_view> {};
    for (auto const [index, row]: core::views::enumerate(StartupMarkTable))
    {
        CHECK(static_cast<size_t>(row.mark) == static_cast<size_t>(index));
        CHECK_FALSE(row.name.empty());
        names.insert(row.name);
    }
    CHECK(names.size() == StartupMarkTable.size());
}
