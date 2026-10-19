// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/PathProbe.hpp>

#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <latch>
#include <ranges>
#include <thread>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;
using core::platform::testing::InMemoryFileSystem;

namespace
{
PathQuery query(std::initializer_list<char const*> candidates)
{
    auto result = PathQuery {};
    for (auto const* candidate: candidates)
        result.candidates.emplace_back(candidate);
    return result;
}

bool sameAnswer(ResolvedPath const& lhs, ResolvedPath const& rhs)
{
    return lhs.path == rhs.path && lhs.kind == rhs.kind && lhs.candidateIndex == rhs.candidateIndex;
}
} // namespace

TEST_CASE("PathProbe.answersFromTheCacheOnlyOnceProbed", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/w/README.md", "");
    fileSystem.addDirectory("/w/src");
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits {} };

    auto const readme = query({ "/v/README.md", "/w/README.md" });
    CHECK_FALSE(probe.cached(readme).has_value());
    auto const found = probe.probe(readme);
    CHECK(found.kind == PathKind::File);
    CHECK(found.path == "/w/README.md");
    CHECK(found.candidateIndex == 1);
    CHECK(probe.cached(readme)->kind == PathKind::File);

    CHECK(probe.probe(query({ "/w/src" })).kind == PathKind::Directory);
    CHECK(probe.probe(query({ "/w/none" })).kind == PathKind::Missing);
    CHECK(probe.cached(query({ "/w/none" }))->kind == PathKind::Missing); // a known miss is an answer too
}

TEST_CASE("PathProbe.reprobesAnAnswerOlderThanItsMaxAge", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits { .capacity = 16, .maxAge = 10s } };

    auto const later = query({ "/w/later.txt" });
    CHECK(probe.probe(later).kind == PathKind::Missing);
    fileSystem.addFile("/w/later.txt", "");
    CHECK(probe.probe(later).kind == PathKind::Missing); // still fresh
    clock.advance(11s);
    CHECK_FALSE(probe.cached(later).has_value());
    CHECK(probe.probe(later).kind == PathKind::File);
}

TEST_CASE("PathProbe.evictsTheLeastRecentlyUsed", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits { .capacity = 2, .maxAge = 10s } };
    (void) probe.probe(query({ "/a" }));
    (void) probe.probe(query({ "/b" }));
    (void) probe.probe(query({ "/c" }));
    CHECK_FALSE(probe.cached(query({ "/a" })).has_value());
    CHECK(probe.cached(query({ "/c" })).has_value());
}

TEST_CASE("PathProbe.probeAsyncRunsOnTheExecutor", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/w/x", "");
    auto clock = core::platform::ManualClock {};
    auto probe = std::make_shared<PathProbe>(fileSystem, clock, PathProbeLimits {});
    auto executor = core::async::testing::ManualExecutor {};

    auto answer = std::optional<ResolvedPath> {};
    probeAsync(probe, executor, query({ "/w/x" }), [&](ResolvedPath result) { answer = std::move(result); });
    CHECK_FALSE(answer.has_value()); // nothing ran on the calling thread
    executor.drain();
    REQUIRE(answer.has_value());
    CHECK(answer->kind == PathKind::File);
}

TEST_CASE("PathProbe.concurrentReadersAgreeWithTheCache", "[links][threading]")
{
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/w/a.txt", "");
    fileSystem.addDirectory("/w/dir");
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits {} };

    // Warm the cache on the main thread; the clock is never advanced, so every answer stays fresh.
    auto const queries = std::vector<PathQuery> {
        query({ "/w/a.txt" }),
        query({ "/w/none" }),
        query({ "/w/dir" }),
        query({ "/w/none", "/w/a.txt" }),
    };
    auto expected = std::vector<ResolvedPath> {};
    for (auto const& q: queries)
        expected.push_back(probe.probe(q));

    // The filesystem is not thread-safe and is never touched from here on: all lookups are cache hits.
    // Each worker walks the queries in a different rotation, so tryGet() reorders the LRU list concurrently.
    // The gate starts both workers together: neither begins its loop before the other has started too.
    auto mismatches = std::vector<std::size_t>(2, std::size_t { 0 });
    auto gate = std::latch { static_cast<std::ptrdiff_t>(mismatches.size()) };
    auto workers = std::vector<std::thread> {};
    for (auto const worker: std::views::iota(std::size_t { 0 }, mismatches.size()))
    {
        workers.emplace_back([&, worker] {
            gate.arrive_and_wait();
            for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
            {
                for (auto const offset: std::views::iota(std::size_t { 0 }, queries.size()))
                {
                    auto const index = (offset + worker) % queries.size();
                    auto const cached = probe.cached(queries[index]);
                    if (!cached || !sameAnswer(*cached, expected[index]))
                        ++mismatches[worker];
                    if (!sameAnswer(probe.probe(queries[index]), expected[index]))
                        ++mismatches[worker];
                }
            }
        });
    }
    for (auto& thread: workers)
        thread.join();

    for (auto const count: mismatches)
        CHECK(count == 0);
}

TEST_CASE("PathProbe.emptyQueryIsMissingWithoutLooking", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits {} };

    auto const empty = PathQuery {};
    REQUIRE(probe.cached(empty).has_value());
    CHECK(probe.cached(empty)->kind == PathKind::Missing);
    CHECK(probe.probe(empty).kind == PathKind::Missing);
}

TEST_CASE("PathProbe.ageBoundaryIsExactlyMaxAge", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits { .capacity = 16, .maxAge = 10s } };

    auto const later = query({ "/w/boundary.txt" });
    CHECK(probe.probe(later).kind == PathKind::Missing);
    fileSystem.addFile("/w/boundary.txt", "");
    clock.advance(10s); // exactly maxAge old: still fresh
    CHECK(probe.cached(later).has_value());
    CHECK(probe.probe(later).kind == PathKind::Missing);
    clock.advance(1ns); // one tick older: stale
    CHECK_FALSE(probe.cached(later).has_value());
    CHECK(probe.probe(later).kind == PathKind::File);
}

TEST_CASE("PathProbe.staleEarlierCandidateHidesAFreshLaterOne", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/w/found.txt", "");
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits { .capacity = 16, .maxAge = 10s } };

    auto const both = query({ "/w/early.txt", "/w/found.txt" });
    CHECK(probe.probe(query({ "/w/early.txt" })).kind == PathKind::Missing); // t = 0
    clock.advance(5s);
    CHECK(probe.probe(query({ "/w/found.txt" })).kind == PathKind::File); // t = 5
    clock.advance(6s);                                                    // early is 11s old, found 6s
    CHECK(probe.cached(query({ "/w/found.txt" })).has_value());
    CHECK_FALSE(probe.cached(both).has_value());
    auto const answer = probe.probe(both);
    CHECK(answer.kind == PathKind::File);
    CHECK(answer.candidateIndex == 1);
}

TEST_CASE("PathProbe.zeroCapacityBehavesLikeOne", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    auto clock = core::platform::ManualClock {};
    auto probe = PathProbe { fileSystem, clock, PathProbeLimits { .capacity = 0, .maxAge = 10s } };

    (void) probe.probe(query({ "/a" }));
    CHECK(probe.cached(query({ "/a" })).has_value());
    (void) probe.probe(query({ "/b" }));
    CHECK_FALSE(probe.cached(query({ "/a" })).has_value());
    CHECK(probe.cached(query({ "/b" })).has_value());
}

TEST_CASE("PathProbe.abandonedJobFreesTheProbeWithoutAnswering", "[links]")
{
    auto fileSystem = InMemoryFileSystem {};
    fileSystem.addFile("/w/x", "");
    auto clock = core::platform::ManualClock {};
    auto probe = std::make_shared<PathProbe>(fileSystem, clock, PathProbeLimits {});
    auto const watch = std::weak_ptr<PathProbe> { probe };

    auto answered = false;
    {
        auto executor = core::async::testing::ManualExecutor {};
        probeAsync(
            std::move(probe), executor, query({ "/w/x" }), [&](ResolvedPath const&) { answered = true; });
    } // destroyed with the job still queued
    CHECK_FALSE(answered);
    CHECK(watch.expired());
}
