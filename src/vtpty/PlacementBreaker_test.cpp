// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PlacementBreaker.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <expected>
#include <string_view>

using namespace std::chrono_literals;
using vtpty::PlacementBreaker;
using vtpty::ScopeError;

namespace
{

constexpr auto Cooldown = 30s;
constexpr auto Start = core::platform::SteadyTimePoint {} + 1h;

using Outcome = std::expected<void, ScopeError>;

struct OutcomeRow
{
    std::string_view name;
    Outcome outcome;
    bool attemptsBeforeCooldown;
};

// Any answer from systemd, a refusal included, shows the bus is alive; only silence holds off.
auto const outcomeRows = std::array {
    OutcomeRow { .name = "placed", .outcome = Outcome {}, .attemptsBeforeCooldown = true },
    OutcomeRow {
        .name = "refused", .outcome = std::unexpected(ScopeError::Refused), .attemptsBeforeCooldown = true },
    OutcomeRow { .name = "unknown property",
                 .outcome = std::unexpected(ScopeError::UnknownProperty),
                 .attemptsBeforeCooldown = true },
    OutcomeRow { .name = "timed out",
                 .outcome = std::unexpected(ScopeError::TimedOut),
                 .attemptsBeforeCooldown = false },
    OutcomeRow { .name = "disconnected",
                 .outcome = std::unexpected(ScopeError::Disconnected),
                 .attemptsBeforeCooldown = false },
    OutcomeRow { .name = "unavailable",
                 .outcome = std::unexpected(ScopeError::Unavailable),
                 .attemptsBeforeCooldown = false },
};

} // namespace

TEST_CASE("PlacementBreaker.outcomes", "[placement][breaker]")
{
    for (auto const& row: outcomeRows)
    {
        INFO("outcome " << row.name);
        auto breaker = PlacementBreaker { Cooldown };
        breaker.record(row.outcome, Start);
        CHECK(breaker.shouldAttempt(Start + 1ms) == row.attemptsBeforeCooldown);
        CHECK(breaker.shouldAttempt(Start + Cooldown));
    }
}

TEST_CASE("PlacementBreaker.attemptsUntilTheBusFallsSilent", "[placement][breaker]")
{
    auto const breaker = PlacementBreaker { Cooldown };
    CHECK(breaker.shouldAttempt(Start));
}

TEST_CASE("PlacementBreaker.aFailedTrialRestartsTheCooldown", "[placement][breaker]")
{
    auto breaker = PlacementBreaker { Cooldown };
    breaker.record(std::unexpected(ScopeError::TimedOut), Start);
    REQUIRE(breaker.shouldAttempt(Start + Cooldown));
    breaker.record(std::unexpected(ScopeError::TimedOut), Start + Cooldown);
    CHECK_FALSE(breaker.shouldAttempt(Start + Cooldown + Cooldown - 1ms));
    CHECK(breaker.shouldAttempt(Start + Cooldown + Cooldown));
}

TEST_CASE("PlacementBreaker.aSuccessfulTrialResumes", "[placement][breaker]")
{
    auto breaker = PlacementBreaker { Cooldown };
    breaker.record(std::unexpected(ScopeError::TimedOut), Start);
    breaker.record(Outcome {}, Start + Cooldown);
    CHECK(breaker.shouldAttempt(Start + Cooldown + 1ms));
}
