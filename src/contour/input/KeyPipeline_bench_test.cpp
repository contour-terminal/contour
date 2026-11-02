// SPDX-License-Identifier: Apache-2.0
//
// The latency of one key press through the pipeline: normalize() and route(), per platform kind, over the
// characterization rows. Hidden (`[.][bench]`): run it on a release build with `contour_gui_test "[bench]"`;
// with CONTOUR_BENCH_ENFORCE set it fails when the 99th percentile exceeds one microsecond.

#include <contour/config/Config.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/input/KeyRouter.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/session/KeyCharacterizationRows.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <variant>
#include <vector>

namespace
{
using contour::config::KeyboardPlatformKind;
using contour::input::KeyEvent;
using contour::input::RouteContext;
using contour::input::SuperEncoding;
using Clock = std::chrono::steady_clock;

/// How many normalize+route round trips are timed per platform kind.
constexpr auto Iterations = std::size_t { 100'000 };

/// The budget for the 99th percentile of one round trip.
constexpr auto P99Budget = std::chrono::nanoseconds { 1000 };

/// @param kind The platform kind whose layout is wanted.
/// @return The layout its key path reads native key codes with.
[[nodiscard]] contour::input::KeyboardLayout const& layoutFor(KeyboardPlatformKind kind)
{
    static auto const mac = contour::input::makeMacUsAnsiKeyboardLayout();
    static auto const win32 = contour::input::makeWin32UsKeyboardLayout();
    static auto const keymapRows =
        std::array { contour::input::FixedXkbKey { .keycode = 12, .group = 0, .keysym = 0x33 } };
    static auto const xkb =
        contour::input::makeXkbKeyboardLayout(contour::input::makeFixedXkbKeymap(keymapRows));
    switch (kind)
    {
        case KeyboardPlatformKind::Mac: return *mac;
        case KeyboardPlatformKind::Win32: return *win32;
        case KeyboardPlatformKind::Xkb: break;
    }
    return *xkb;
}

/// Routes @p event as the session does: a named key once, or each character in turn.
/// @return A value that depends on the routes, so that the optimizer cannot drop them.
[[nodiscard]] std::size_t routeEvent(KeyEvent const& event, RouteContext const& context)
{
    auto sink = std::size_t { 0 };
    if (event.key)
    {
        auto const route = contour::input::routeKey(*event.key, event.modifiers, event.appModifier, context);
        sink += route.otherwise.index();
        return sink;
    }
    for (auto const codepoint: event.text)
    {
        auto const route =
            contour::input::routeChar(codepoint, event.identity, event.modifiers, event.appModifier, context);
        sink += route.otherwise.index();
    }
    return sink;
}

[[nodiscard]] std::chrono::nanoseconds percentile(std::vector<std::chrono::nanoseconds> const& sorted,
                                                  std::size_t per100)
{
    return sorted[std::min(sorted.size() - 1, sorted.size() * per100 / 100)];
}
} // namespace

TEST_CASE("a key press normalizes and routes within a microsecond", "[.][bench]")
{
    auto const config = contour::test::loadConfigFromYaml(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)");
    auto const enforce = core::defaultEnvironment().get("CONTOUR_BENCH_ENFORCE").has_value();
    auto sink = std::size_t { 0 };

    for (auto const kind:
         { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32, KeyboardPlatformKind::Mac })
    {
        auto const platform = contour::input::makeKeyboardPlatform(kind, {}, layoutFor(kind));
        auto const context =
            RouteContext { .config = config, .modes = 0, .superEncoding = SuperEncoding::Unrepresentable };

        auto events = std::vector<contour::input::NativeKeyEvent> {};
        for (auto const& row: contour::test::keyCharacterizationRows())
            if (row.kind == kind)
                events.push_back(row.event);
        REQUIRE_FALSE(events.empty());

        auto durations = std::vector<std::chrono::nanoseconds> {};
        durations.reserve(Iterations);
        for (auto const iteration: std::views::iota(std::size_t { 0 }, Iterations))
        {
            auto const& native = events[iteration % events.size()];
            auto const start = Clock::now();
            auto const event = platform->normalize(native);
            sink += routeEvent(event, context);
            durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start));
        }
        std::ranges::sort(durations);

        auto const p50 = percentile(durations, 50);
        auto const p99 = percentile(durations, 99);
        std::cout << std::format("key pipeline kind={} rows={} p50={}ns p99={}ns\n",
                                 static_cast<int>(kind),
                                 events.size(),
                                 p50.count(),
                                 p99.count());
        if (enforce)
            CHECK(p99 <= P99Budget);
    }
    std::cout << std::format("sink={}\n", sink);
}
