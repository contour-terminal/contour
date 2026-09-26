// SPDX-License-Identifier: Apache-2.0
#include <core/net/IAdmissionControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <cstddef>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

using core::net::AdmissionLease;
using core::net::CountingAdmissionControl;

TEST_CASE("a counting policy admits up to its cap and no further", "[net]")
{
    auto policy = CountingAdmissionControl { 2 };

    auto first = policy.tryAdmit();
    auto second = policy.tryAdmit();
    auto third = policy.tryAdmit();

    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK_FALSE(third.has_value());
    CHECK(policy.inFlight() == 2);
}

TEST_CASE("a lease gives its slot back when it ends, and only then", "[net]")
{
    // The lease is the whole reason `onConnectionEnded` is gone: a slot handed back by a call is
    // one a caller can forget to make, or make twice — and one without a matching start used to
    // wrap the counter to SIZE_MAX, after which every admission was refused for good.
    auto policy = CountingAdmissionControl { 1 };

    auto held = policy.tryAdmit();
    REQUIRE(held.has_value());
    CHECK_FALSE(policy.tryAdmit().has_value());

    held.reset();
    CHECK(policy.inFlight() == 0);
    CHECK(policy.tryAdmit().has_value());
}

TEST_CASE("a moved lease gives its slot back once, not twice", "[net]")
{
    auto policy = CountingAdmissionControl { 2 };

    auto leases = std::vector<AdmissionLease> {};
    {
        auto admitted = policy.tryAdmit();
        REQUIRE(admitted.has_value());
        leases.push_back(std::move(*admitted));
    } // the moved-from lease ends here and must give nothing back
    CHECK(policy.inFlight() == 1);

    auto other = policy.tryAdmit();
    REQUIRE(other.has_value());
    CHECK(policy.inFlight() == 2);

    leases.clear();
    other.reset();
    CHECK(policy.inFlight() == 0);
}

TEST_CASE("a cap of zero admits everything and still counts", "[net]")
{
    auto policy = CountingAdmissionControl { 0 };

    auto leases = std::vector<AdmissionLease> {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 1000))
    {
        auto admitted = policy.tryAdmit();
        REQUIRE(admitted.has_value());
        leases.push_back(std::move(*admitted));
    }
    CHECK(policy.inFlight() == 1000);
    leases.clear();
    CHECK(policy.inFlight() == 0);
}

TEST_CASE("two threads racing for the last slot cannot both be admitted", "[net]")
{
    // **The race this interface exists to close.** Two loops accept on one policy; the cap is
    // two and one connection is already in flight. Asked as `allowAccept()` followed by
    // `onConnectionStarted()`, both loops can read "one in flight" before either counts itself,
    // and both admit: a cap exceeded under exactly the load it exists for. `tryAdmit` is one
    // atomic step, so of two simultaneous callers exactly one wins.
    //
    // Two workers are held at a barrier and released together, round after round, so the window
    // between the two halves of a check-then-act is exercised thousands of times rather than
    // hoped for once. A round that admits both, or neither, is a violation.
    constexpr auto Rounds = 20000;
    auto policy = CountingAdmissionControl { 2 };
    auto const base = policy.tryAdmit();
    REQUIRE(base.has_value());

    auto results = std::array<std::optional<AdmissionLease>, 2> {};
    auto sync = std::barrier { 3 };

    auto contend = [&](std::size_t slot) {
        for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
        {
            sync.arrive_and_wait(); // start together
            results[slot] = policy.tryAdmit();
            sync.arrive_and_wait(); // done; the main thread reads and clears
        }
    };
    auto first = std::thread { contend, std::size_t { 0 } };
    auto second = std::thread { contend, std::size_t { 1 } };

    auto violations = 0;
    auto overCap = 0;
    for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
    {
        sync.arrive_and_wait();
        sync.arrive_and_wait();
        auto const admitted =
            static_cast<int>(results[0].has_value()) + static_cast<int>(results[1].has_value());
        if (admitted != 1)
            ++violations;
        if (policy.inFlight() > 2)
            ++overCap;
        results[0].reset();
        results[1].reset();
    }
    first.join();
    second.join();

    INFO(violations << " of " << Rounds << " rounds did not admit exactly one of two racing callers");
    CHECK(violations == 0);
    CHECK(overCap == 0);
    CHECK(policy.inFlight() == 1);
}
