// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <vthost/ClientCensus.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/testing/FakeDaemonControl.hpp>

namespace
{

/// Records every count a census reports, in order.
struct CountLog final: vthost::CensusEvents
{
    std::vector<std::size_t> counts;
    void clientCountChanged(std::size_t count) override { counts.push_back(count); }
};

/// What a wrapped handler saw of the census while it ran.
struct Observed
{
    std::size_t count = 0;
};

/// A connection that only notes how many clients the census counts while it runs.
core::async::Task<void> noteCount(vthost::ClientCensus const* census, Observed* observed)
{
    observed->count = census->count();
    co_return;
}

} // namespace

TEST_CASE("the null daemon control serves nothing and counts no one", "[vthost][census]")
{
    auto& control = vthost::nullDaemonControl();
    CHECK(control.status().error() == vthost::DaemonControlError::NotServed);
    CHECK(control.requestStop(vthost::proto::StopReason::User).error()
          == vthost::DaemonControlError::NotServed);
    CHECK(control.releaseVersion().empty());
    CHECK(&control == &vthost::nullDaemonControl());            // one shared instance
    [[maybe_unused]] auto const ticket = control.admitClient(); // an empty ticket: nothing to release
}

TEST_CASE("countedHandler holds a census ticket exactly while the connection runs", "[vthost][census]")
{
    auto control = vthost::testing::FakeDaemonControl {};
    auto log = CountLog {};
    control.census.subscribe(&log);
    auto observed = Observed {};
    auto const handler = vthost::countedHandler(
        control, [&](vthost::ConnectionId const&, std::unique_ptr<core::net::ISocket> const&) {
            return noteCount(&control.census, &observed);
        });

    auto backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    loop.blockOn(handler(vthost::ConnectionId { .endpoint = "test", .index = 1 }, nullptr));

    CHECK(observed.count == 1);
    CHECK(control.census.count() == 0);
    CHECK(log.counts == std::vector<std::size_t> { 1, 0 });
    control.census.unsubscribe(&log);
}

TEST_CASE("a moved census ticket is counted once", "[vthost][census]")
{
    auto census = vthost::ClientCensus {};
    auto a = census.admit();
    auto b = std::move(a);
    CHECK(census.count() == 1);
    {
        auto const dropped = std::move(b);
    }
    CHECK(census.count() == 0);
}

TEST_CASE("a census tells its subscribers every change, and unsubscribing twice is harmless",
          "[vthost][census]")
{
    auto census = vthost::ClientCensus {};
    auto log = CountLog {};
    census.subscribe(&log);
    {
        auto const first = census.admit();
        auto second = census.admit();
        auto assigned = vthost::ClientCensus::Ticket {};
        assigned = std::move(second); // moves the holding, it counts no one more
        CHECK(census.count() == 2);
    }
    census.unsubscribe(&log);
    census.unsubscribe(&log);
    {
        [[maybe_unused]] auto const unheard = census.admit();
    }
    CHECK(log.counts == std::vector<std::size_t> { 1, 2, 1, 0 });
}
