// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/MarkArbiter.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("MarkArbiter.a fresh session lets OSC 3008 mark but not notify", "[markarbiter]")
{
    // Flags are idempotent bits on a logical line head, so stamping one that OSC 133 may also stamp
    // costs nothing. Callbacks are not idempotent, so they wait for the decision.
    auto const arbiter = MarkArbiter {};
    CHECK(arbiter.owner() == MarkOwner::Undecided);
    CHECK(arbiter.contextMayMark());
    CHECK(!arbiter.contextMayNotify());
}

TEST_CASE("MarkArbiter.the first OSC 133 sequence settles the session", "[markarbiter]")
{
    auto arbiter = MarkArbiter {};
    arbiter.observedShellIntegration();

    CHECK(arbiter.owner() == MarkOwner::ShellIntegration);
    CHECK(!arbiter.contextMayMark());
    CHECK(!arbiter.contextMayNotify());
}

TEST_CASE("MarkArbiter.one completed 3008 command cycle hands over the session", "[markarbiter]")
{
    auto arbiter = MarkArbiter {};
    arbiter.observedContextCommandStart();
    CHECK(arbiter.owner() == MarkOwner::Undecided);
    CHECK(!arbiter.contextMayNotify()); // still mid-cycle: OSC 133 could yet appear

    arbiter.observedContextCommandEnd();

    CHECK(arbiter.owner() == MarkOwner::ContextSignalling);
    CHECK(arbiter.contextMayMark());
    CHECK(arbiter.contextMayNotify());
}

TEST_CASE("MarkArbiter.an end without a matching start does not complete a cycle", "[markarbiter]")
{
    // The decision is on a cycle BOUNDARY, so a stray end -- one whose start was refused by the depth
    // limit, say -- must not stand in for a whole cycle.
    auto arbiter = MarkArbiter {};
    arbiter.observedContextCommandEnd();

    CHECK(arbiter.owner() == MarkOwner::Undecided);
    CHECK(!arbiter.contextMayNotify());
}

TEST_CASE("MarkArbiter.OSC 133 arriving mid-cycle still wins", "[markarbiter]")
{
    auto arbiter = MarkArbiter {};
    arbiter.observedContextCommandStart();
    arbiter.observedShellIntegration();
    arbiter.observedContextCommandEnd();

    CHECK(arbiter.owner() == MarkOwner::ShellIntegration);
    CHECK(!arbiter.contextMayMark());
}

TEST_CASE("MarkArbiter.OSC 133 sourced mid-session supersedes context signalling", "[markarbiter]")
{
    // A user may `source` a shell integration halfway through a session. That has to take effect.
    auto arbiter = MarkArbiter {};
    arbiter.observedContextCommandStart();
    arbiter.observedContextCommandEnd();
    REQUIRE(arbiter.owner() == MarkOwner::ContextSignalling);

    arbiter.observedShellIntegration();

    CHECK(arbiter.owner() == MarkOwner::ShellIntegration);
    CHECK(!arbiter.contextMayMark());
}

TEST_CASE("MarkArbiter.context signalling never takes the marks back", "[markarbiter]")
{
    // The whole hysteresis. Without it a session would flap between marker sources depending on which
    // stream happened to be quiet.
    auto arbiter = MarkArbiter {};
    arbiter.observedShellIntegration();

    for (auto i = 0; i < 5; ++i)
    {
        arbiter.observedContextCommandStart();
        arbiter.observedContextCommandEnd();
        CHECK(arbiter.owner() == MarkOwner::ShellIntegration);
        CHECK(!arbiter.contextMayMark());
    }
}

TEST_CASE("MarkArbiter.ContextMarkPolicy::Never silences OSC 3008 entirely", "[markarbiter]")
{
    auto arbiter = MarkArbiter { ContextMarkPolicy::Never };
    arbiter.observedContextCommandStart();
    arbiter.observedContextCommandEnd();

    // The owner still moves -- the policy gates the ACT, not the observation -- but nothing is stamped.
    CHECK(!arbiter.contextMayMark());
    CHECK(!arbiter.contextMayNotify());
}

TEST_CASE("MarkArbiter.repeated cycles keep the session with context signalling", "[markarbiter]")
{
    auto arbiter = MarkArbiter {};
    for (auto i = 0; i < 5; ++i)
    {
        arbiter.observedContextCommandStart();
        arbiter.observedContextCommandEnd();
    }
    CHECK(arbiter.owner() == MarkOwner::ContextSignalling);
    CHECK(arbiter.contextMayNotify());
}
