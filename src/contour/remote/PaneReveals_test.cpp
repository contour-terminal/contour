// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/PaneReveals.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <ranges>
#include <vector>

using namespace std::chrono_literals;
using contour::remote::followReveals;
using contour::remote::PendingReveal;
using contour::remote::RevealLooks;
using contour::remote::RevealPatience;

namespace
{
/// A point on the reveals' clock.
constexpr auto Start = core::platform::SteadyTimePoint {} + 100s;

/// @return A reveal of @p session that arrived at @p arrived.
[[nodiscard]] PendingReveal revealOf(uint64_t session, core::platform::SteadyTimePoint arrived = Start)
{
    return PendingReveal { .session = session, .arrived = arrived, .looksLeft = RevealLooks };
}
} // namespace

TEST_CASE("followReveals: a shown pane is revealed, once", "[remote][presence]")
{
    auto revealed = std::vector<uint64_t> {};
    auto const waiting = followReveals({ revealOf(7) }, Start, [&](uint64_t session) {
        revealed.push_back(session);
        return true;
    });
    CHECK(revealed == std::vector<uint64_t> { 7 });
    CHECK(waiting.empty());
}

TEST_CASE("followReveals: a pane not shown yet waits a few looks, then is dropped", "[remote][presence]")
{
    auto const notShown = [](uint64_t) {
        return false;
    };
    auto pending = std::vector { revealOf(7) };
    for ([[maybe_unused]] auto const look: std::views::iota(0, RevealLooks))
    {
        pending = followReveals(pending, Start + 10ms, notShown);
        REQUIRE(pending.size() == 1);
    }
    CHECK(followReveals(pending, Start + 10ms, notShown).empty());
}

TEST_CASE("followReveals: a reveal older than its patience is dropped, however many looks it has left",
          "[remote][presence]")
{
    auto shownCalls = 0;
    auto const shown = [&](uint64_t) {
        ++shownCalls;
        return true;
    };
    // It arrived while the link was not live, and the window shows its pane only now: too late to steal the
    // focus.
    CHECK(followReveals({ revealOf(7, Start) }, Start + RevealPatience + 1ms, shown).empty());
    CHECK(shownCalls == 0);
    // Within its patience, it is carried out.
    CHECK(followReveals({ revealOf(8, Start) }, Start + RevealPatience, shown).empty());
    CHECK(shownCalls == 1);
}
