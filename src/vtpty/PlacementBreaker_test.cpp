// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PlacementBreaker.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <expected>
#include <string_view>

using namespace std::chrono_literals;
using vtpty::BreakerState;
using vtpty::PlacementBreaker;
using vtpty::ScopeError;

namespace
{

constexpr auto Cooldown = 30s;
constexpr auto Start = PlacementBreaker::Clock::time_point {} + 1h;

using Outcome = std::expected<void, ScopeError>;

/// @return A breaker driven into @p state at Start.
PlacementBreaker breakerIn(BreakerState state)
{
    auto breaker = PlacementBreaker { Cooldown };
    if (state == BreakerState::Closed)
        return breaker;
    REQUIRE(breaker.shouldAttempt(Start));
    breaker.record(std::unexpected(ScopeError::TimedOut), Start);
    if (state == BreakerState::HalfOpen)
        REQUIRE(breaker.shouldAttempt(Start + Cooldown));
    REQUIRE(breaker.state() == state);
    return breaker;
}

struct OutcomeRow
{
    std::string_view name;
    Outcome outcome;
    BreakerState after;
};

// Any answer from systemd, a refusal included, shows the bus is alive; only silence opens.
auto const outcomeRows = std::array {
    OutcomeRow { .name = "placed", .outcome = Outcome {}, .after = BreakerState::Closed },
    OutcomeRow {
        .name = "refused", .outcome = std::unexpected(ScopeError::Refused), .after = BreakerState::Closed },
    OutcomeRow { .name = "unknown property",
                 .outcome = std::unexpected(ScopeError::UnknownProperty),
                 .after = BreakerState::Closed },
    OutcomeRow {
        .name = "timed out", .outcome = std::unexpected(ScopeError::TimedOut), .after = BreakerState::Open },
    OutcomeRow { .name = "disconnected",
                 .outcome = std::unexpected(ScopeError::Disconnected),
                 .after = BreakerState::Open },
    OutcomeRow { .name = "unavailable",
                 .outcome = std::unexpected(ScopeError::Unavailable),
                 .after = BreakerState::Open },
};

} // namespace

TEST_CASE("PlacementBreaker.outcomes", "[placement][breaker]")
{
    for (auto const from: { BreakerState::Closed, BreakerState::HalfOpen })
    {
        for (auto const& row: outcomeRows)
        {
            INFO("from " << static_cast<int>(from) << ", outcome " << row.name);
            auto breaker = breakerIn(from);
            breaker.record(row.outcome, Start + Cooldown);
            CHECK(breaker.state() == row.after);
        }
    }
}

TEST_CASE("PlacementBreaker.cooldown", "[placement][breaker]")
{
    auto breaker = breakerIn(BreakerState::Open);
    CHECK_FALSE(breaker.shouldAttempt(Start + Cooldown - 1ms));
    CHECK(breaker.state() == BreakerState::Open);
    CHECK(breaker.shouldAttempt(Start + Cooldown));
    CHECK(breaker.state() == BreakerState::HalfOpen);
}

TEST_CASE("PlacementBreaker.reopenRestartsTheCooldown", "[placement][breaker]")
{
    auto breaker = breakerIn(BreakerState::HalfOpen);
    breaker.record(std::unexpected(ScopeError::TimedOut), Start + Cooldown);
    CHECK_FALSE(breaker.shouldAttempt(Start + Cooldown + Cooldown - 1ms));
    CHECK(breaker.shouldAttempt(Start + Cooldown + Cooldown));
}

TEST_CASE("PlacementBreaker.closedAlwaysAttempts", "[placement][breaker]")
{
    auto breaker = PlacementBreaker { Cooldown };
    CHECK(breaker.shouldAttempt(Start));
    breaker.record(std::unexpected(ScopeError::Refused), Start);
    CHECK(breaker.shouldAttempt(Start));
}

TEST_CASE("ScopeError.traitsTableMatchesTheEnum", "[placement]")
{
    for (auto const error: { ScopeError::Unavailable,
                             ScopeError::Disconnected,
                             ScopeError::TimedOut,
                             ScopeError::Refused,
                             ScopeError::UnknownProperty })
    {
        CHECK(vtpty::traitsOf(error).error == error);
        CHECK_FALSE(vtpty::traitsOf(error).description.empty());
    }
    CHECK(vtpty::traitsOf(ScopeError::Unavailable).severity == vtpty::ScopeErrorSeverity::Expected);
    CHECK(vtpty::traitsOf(ScopeError::Refused).severity == vtpty::ScopeErrorSeverity::Fault);
}
