// SPDX-License-Identifier: Apache-2.0
#include <core/async/StopToken.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <version>

// Single-threaded WebAssembly has no threads to start (Part I §1).
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #define CORE_CPP_TEST_THREADS 1
    #include <atomic>
    #include <chrono>
    #include <thread>
    #include <vector>
#else
    #define CORE_CPP_TEST_THREADS 0
#endif

using core::async::NoStopState;
using core::async::StopCallback;
using core::async::StopSource;
using core::async::StopToken;

// StopToken.hpp is included first, before anything could have defined __cpp_lib_jthread for it.
// Where the standard library has std::stop_token, StopToken must be it all the same, unless the
// build forces the fallback (core-cpp.async-fallback): the choice may not depend on what a
// translation unit happened to include first.
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L \
    && !defined(CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK)
    #include <stop_token>
static_assert(std::is_same_v<StopToken, std::stop_token>);
static_assert(std::is_same_v<StopSource, std::stop_source>);
static_assert(std::is_same_v<StopCallback<void (*)()>, std::stop_callback<void (*)()>>);
static_assert(std::is_same_v<std::remove_const_t<decltype(NoStopState)>, std::nostopstate_t>);
#else
static_assert(std::is_same_v<StopToken, core::async::detail::StopTokenFallback>);
static_assert(std::is_same_v<StopSource, core::async::detail::StopSourceFallback>);
static_assert(
    std::is_same_v<StopCallback<void (*)()>, core::async::detail::StopCallbackFallback<void (*)()>>);
static_assert(
    std::is_same_v<std::remove_const_t<decltype(NoStopState)>, core::async::detail::NoStopStateFallback>);
#endif

// What code written against one branch relies on from the other.
static_assert(std::is_nothrow_copy_constructible_v<StopToken>);
static_assert(std::is_nothrow_move_constructible_v<StopToken>);
static_assert(std::is_nothrow_copy_constructible_v<StopSource>);
static_assert(std::is_nothrow_move_constructible_v<StopSource>);
static_assert(!std::is_copy_constructible_v<StopCallback<void (*)()>>);
static_assert(!std::is_move_constructible_v<StopCallback<void (*)()>>);
static_assert(std::is_same_v<StopCallback<void (*)()>::callback_type, void (*)()>);
static_assert(!std::is_convertible_v<decltype(NoStopState), StopSource>,
              "the StopSource constructor is explicit");

namespace
{

/// Counts its invocations into a counter that outlives it.
struct Count
{
    int* calls;

    void operator()() const noexcept { ++*calls; }
};

/// Callable only as an rvalue. std::stop_callback invokes its callback as
/// `std::forward<Callback>(callback)()`, so code written against it may rely on that.
struct CallAsRvalue
{
    int* calls;

    void operator()() const&& noexcept { ++*calls; }
};

/// Destroys the StopCallback that holds it, from inside its own invocation.
struct DestroySelf
{
    std::optional<StopCallback<DestroySelf>>* self;
    int* calls;

    void operator()() const
    {
        ++*calls;
        self->reset();
    }
};

/// Destroys another StopCallback, from inside its own invocation.
struct DestroyOther
{
    std::optional<StopCallback<DestroyOther>>* other;
    int* calls;

    void operator()() const
    {
        ++*calls;
        other->reset();
    }
};

/// Destroys the last source of its stop state and then the fallback callback that holds it, from
/// inside its own invocation, which the source's request_stop is running.
struct DestroySourceAndSelf
{
    std::optional<core::async::detail::StopSourceFallback>* source;
    std::optional<core::async::detail::StopCallbackFallback<DestroySourceAndSelf>>* self;
    int* calls;

    void operator()() const
    {
        ++*calls;
        source->reset();
        self->reset(); // Destroys this object too: nothing of it is touched after this.
    }
};

} // namespace

TEST_CASE("A default-constructed StopToken has no stop state", "[StopToken]")
{
    auto const token = StopToken {};

    CHECK_FALSE(token.stop_possible());
    CHECK_FALSE(token.stop_requested());
    CHECK(token == StopToken {});
}

TEST_CASE("request_stop returns true for the first request only", "[StopToken]")
{
    auto source = StopSource {};
    auto const token = source.get_token();
    CHECK(source.stop_possible());
    CHECK(token.stop_possible());
    CHECK_FALSE(source.stop_requested());
    CHECK_FALSE(token.stop_requested());

    CHECK(source.request_stop());
    CHECK(source.stop_requested());
    CHECK(token.stop_requested());

    CHECK_FALSE(source.request_stop());
    CHECK(source.stop_requested());
    CHECK(token.stop_requested());
}

TEST_CASE("request_stop runs every registered callback exactly once", "[StopToken]")
{
    auto source = StopSource {};
    auto firstCalls = 0;
    auto secondCalls = 0;
    auto thirdCalls = 0;
    auto const first = StopCallback<Count> { source.get_token(), Count { &firstCalls } };
    auto const second = StopCallback<Count> { source.get_token(), Count { &secondCalls } };
    auto const third = StopCallback<Count> { source.get_token(), Count { &thirdCalls } };
    CHECK(std::array { firstCalls, secondCalls, thirdCalls } == std::array { 0, 0, 0 });

    CHECK(source.request_stop());
    CHECK(std::array { firstCalls, secondCalls, thirdCalls } == std::array { 1, 1, 1 });

    CHECK_FALSE(source.request_stop());
    CHECK(std::array { firstCalls, secondCalls, thirdCalls } == std::array { 1, 1, 1 });
}

TEST_CASE("A StopCallback constructed on a stopped token runs in its constructor", "[StopToken]")
{
    auto source = StopSource {};
    CHECK(source.request_stop());

    auto calls = 0;
    auto const callback = StopCallback<Count> { source.get_token(), Count { &calls } };
    CHECK(calls == 1);

    CHECK_FALSE(source.request_stop());
    CHECK(calls == 1);
}

TEST_CASE("A destroyed StopCallback is not run and the others still are", "[StopToken]")
{
    constexpr auto CallbackCount = std::size_t { 3 };
    for (auto const destroyed: std::views::iota(std::size_t { 0 }, CallbackCount))
    {
        CAPTURE(destroyed);
        auto source = StopSource {};
        auto calls = std::array<int, CallbackCount> {};
        auto callbacks = std::array<std::optional<StopCallback<Count>>, CallbackCount> {};
        for (auto const i: std::views::iota(std::size_t { 0 }, CallbackCount))
            callbacks[i].emplace(source.get_token(), Count { &calls[i] });

        callbacks[destroyed].reset();
        CHECK(source.request_stop());

        auto expected = std::array { 1, 1, 1 };
        expected[destroyed] = 0;
        CHECK(calls == expected);
    }
}

TEST_CASE("A callback is invoked as an rvalue", "[StopToken]")
{
    auto source = StopSource {};
    auto calls = 0;
    auto const callback = StopCallback<CallAsRvalue> { source.get_token(), CallAsRvalue { &calls } };

    CHECK(source.request_stop());
    CHECK(calls == 1);
}

TEST_CASE("A StopCallback destroyed by its own callback does not wait for itself", "[StopToken]")
{
    auto source = StopSource {};
    auto calls = 0;
    auto callback = std::optional<StopCallback<DestroySelf>> {};
    callback.emplace(source.get_token(), DestroySelf { &callback, &calls });

    CHECK(source.request_stop());
    CHECK(calls == 1);
    CHECK_FALSE(callback.has_value());
}

TEST_CASE("A callback that destroys another registered callback keeps it from running", "[StopToken]")
{
    // Each destroys the other, so the one that runs first finds the other still registered,
    // whichever order the implementation runs them in.
    auto source = StopSource {};
    auto calls = 0;
    auto first = std::optional<StopCallback<DestroyOther>> {};
    auto second = std::optional<StopCallback<DestroyOther>> {};
    first.emplace(source.get_token(), DestroyOther { &second, &calls });
    second.emplace(source.get_token(), DestroyOther { &first, &calls });

    CHECK(source.request_stop());
    CHECK(calls == 1);
    CHECK(first.has_value() != second.has_value());
}

TEST_CASE("The fallback lets a callback destroy the last source and itself while stop runs it", "[StopToken]")
{
    // The fallback's own types, on every platform: the standard does not promise this of
    // std::stop_source. No token remains, so while the callback runs, the source's request_stop is
    // what keeps the stop state alive.
    using core::async::detail::StopCallbackFallback;
    using core::async::detail::StopSourceFallback;
    auto source = std::optional<StopSourceFallback> { std::in_place };
    auto callback = std::optional<StopCallbackFallback<DestroySourceAndSelf>> {};
    auto calls = 0;
    callback.emplace(source->get_token(), DestroySourceAndSelf { &source, &callback, &calls });

    auto const first = source->request_stop();
    CHECK(first);
    CHECK(calls == 1);
    CHECK_FALSE(source.has_value());
    CHECK_FALSE(callback.has_value());
}

TEST_CASE("A callback may request stop and register a callback on the state that runs it", "[StopToken]")
{
    // request_stop holds no lock while a callback runs: either of these would deadlock otherwise.
    auto source = StopSource {};
    auto repeated = std::optional<bool> {};
    auto nestedCalls = 0;
    auto nested = std::optional<StopCallback<Count>> {};
    auto const outer =
        StopCallback<std::function<void()>> { source.get_token(), [&] {
                                                 repeated = source.request_stop();
                                                 nested.emplace(source.get_token(), Count { &nestedCalls });
                                             } };

    CHECK(source.request_stop());
    CHECK(repeated == false);
    CHECK(nestedCalls == 1);
}

TEST_CASE("Copies of a token and of a source share one stop state", "[StopToken]")
{
    auto source = StopSource {};
    auto const token = source.get_token();
    auto tokenCopy = StopToken {};
    tokenCopy = token;
    auto sourceCopy = source;
    CHECK(token == tokenCopy);
    CHECK(source == sourceCopy);
    CHECK(token == sourceCopy.get_token());
    CHECK_FALSE(token == StopSource {}.get_token());

    CHECK(sourceCopy.request_stop());
    CHECK(source.stop_requested());
    CHECK(token.stop_requested());
    CHECK(tokenCopy.stop_requested());
    CHECK_FALSE(source.request_stop());
}

TEST_CASE("stop_possible is false once every source is gone without a request", "[StopToken]")
{
    auto token = StopToken {};
    auto copyOfSource = std::optional<StopSource> {};
    {
        auto const source = StopSource {};
        token = source.get_token();
        copyOfSource.emplace(source);
    }
    CHECK(token.stop_possible()); // the copy is a source of the same state

    copyOfSource.reset();
    CHECK_FALSE(token.stop_possible());
    CHECK_FALSE(token.stop_requested());
}

TEST_CASE("A requested stop stays requested and possible after the sources are gone", "[StopToken]")
{
    auto token = StopToken {};
    {
        auto source = StopSource {};
        token = source.get_token();
        CHECK(source.request_stop());
    }
    CHECK(token.stop_requested());
    CHECK(token.stop_possible());
}

TEST_CASE("A StopSource constructed with NoStopState has no stop state", "[StopToken]")
{
    auto source = StopSource { NoStopState };
    CHECK_FALSE(source.stop_possible());
    CHECK_FALSE(source.stop_requested());
    CHECK_FALSE(source.request_stop());

    auto const token = source.get_token();
    CHECK_FALSE(token.stop_possible());
    CHECK(token == StopToken {});

    auto calls = 0;
    {
        auto const callback = StopCallback<Count> { token, Count { &calls } };
    }
    CHECK(calls == 0);
}

#if CORE_CPP_TEST_THREADS

namespace
{

/// How long a test waits for a step of another thread: generous, for a cold two-core runner
/// under ThreadSanitizer.
constexpr auto WaitBound = std::chrono::seconds { 30 };

/// Waits until @p flag is set, for at most WaitBound on the steady clock.
/// @return Whether it was set in time.
[[nodiscard]] bool waitUntilSet(std::atomic<bool> const& flag)
{
    auto const deadline = std::chrono::steady_clock::now() + WaitBound;
    while (!flag.load())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

} // namespace

TEST_CASE("request_stop runs the callbacks on the requesting thread", "[StopToken][threads]")
{
    auto source = StopSource {};
    auto ranOn = std::thread::id {};
    auto const callback = StopCallback<std::function<void()>> { source.get_token(), [&ranOn] {
                                                                   ranOn = std::this_thread::get_id();
                                                               } };

    auto requester = std::thread { [&source] { static_cast<void>(source.request_stop()); } };
    auto const requesterId = requester.get_id();
    requester.join();

    CHECK(ranOn == requesterId);
    CHECK(ranOn != std::this_thread::get_id());
}

TEST_CASE("Of concurrent request_stop calls exactly one returns true", "[StopToken][threads]")
{
    constexpr auto RequesterCount = 8;
    auto source = StopSource {};
    auto calls = std::atomic<int> { 0 };
    auto const callback = StopCallback<std::function<void()>> { source.get_token(), [&calls] { ++calls; } };

    auto gate = std::atomic<bool> { false };
    auto gateTimedOut = std::atomic<bool> { false };
    auto granted = std::atomic<int> { 0 };
    auto requesters = std::vector<std::thread> {};
    requesters.reserve(RequesterCount);
    for ([[maybe_unused]] auto const i: std::views::iota(0, RequesterCount))
    {
        requesters.emplace_back([&] {
            if (!waitUntilSet(gate))
                gateTimedOut = true;
            if (source.request_stop())
                ++granted;
        });
    }
    gate = true;
    for (auto& requester: requesters)
        requester.join();

    CHECK_FALSE(gateTimedOut.load());
    CHECK(granted.load() == 1);
    CHECK(calls.load() == 1);
}

TEST_CASE("The StopCallback destructor waits for its callback running on another thread",
          "[StopToken][threads]")
{
    auto source = StopSource {};
    auto entered = std::atomic<bool> { false };
    auto destroying = std::atomic<bool> { false };
    auto sawDestroying = std::atomic<bool> { false };
    auto returned = std::atomic<bool> { false };

    auto callback = std::optional<StopCallback<std::function<void()>>> {};
    callback.emplace(source.get_token(), [&] {
        entered = true;
        // Still running once the destructor is on its way, and for a while after it: a destructor
        // that did not wait would return in the meantime.
        sawDestroying = waitUntilSet(destroying);
        std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
        returned = true;
    });

    auto requester = std::thread { [&source] { static_cast<void>(source.request_stop()); } };
    auto const didEnter = waitUntilSet(entered);
    destroying = true;
    callback.reset();
    auto const returnedBeforeDestroyed = returned.load();
    requester.join();

    CHECK(didEnter);
    CHECK(sawDestroying.load());
    CHECK(returnedBeforeDestroyed);
}

TEST_CASE("request_stop racing the StopCallback destructor never runs a destroyed callback",
          "[StopToken][threads]")
{
    // ThreadSanitizer's case: the callback's storage is written by one thread and destroyed by the
    // other, and only the stop state's synchronisation orders the two. The destructor starts once
    // the requesting thread is about to request stop, and a callback that wins the race keeps
    // running until the destructor is on its way; the gate makes it likely, not certain, that the
    // destructor meets it running.
    constexpr auto Rounds = 500;
    auto roundsThatRan = 0;
    auto overRuns = 0;
    auto lateRuns = 0;
    auto gateTimeouts = 0;
    for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
    {
        auto source = StopSource {};
        auto calls = std::atomic<int> { 0 };
        auto requesting = std::atomic<bool> { false };
        auto destroying = std::atomic<bool> { false };
        auto destroyed = std::atomic<bool> { false };
        auto late = std::atomic<bool> { false };
        {
            auto callback = std::optional<StopCallback<std::function<void()>>> {};
            callback.emplace(source.get_token(), [&] {
                ++calls;
                static_cast<void>(waitUntilSet(destroying));
                if (destroyed.load())
                    late = true;
            });
            auto requester = std::thread { [&] {
                requesting = true;
                static_cast<void>(source.request_stop());
            } };
            gateTimeouts += waitUntilSet(requesting) ? 0 : 1;
            destroying = true;
            callback.reset();
            destroyed = true;
            requester.join();
        }
        roundsThatRan += calls.load() == 1 ? 1 : 0;
        overRuns += calls.load() > 1 ? 1 : 0;
        lateRuns += late.load() ? 1 : 0;
    }

    CAPTURE(Rounds, roundsThatRan);
    CHECK(gateTimeouts == 0);
    CHECK(overRuns == 0);
    CHECK(lateRuns == 0);
    if (roundsThatRan == 0)
        SKIP("the requesting thread never reached the callback before its destruction, so no round "
             "raced a running callback against its destructor");
}

TEST_CASE("A StopCallback registered while another thread requests stop runs exactly once",
          "[StopToken][threads]")
{
    // Registration and request_stop decide under one lock whether the callback runs inline or on
    // the requesting thread; it must be one of the two, never both, never neither.
    constexpr auto Rounds = 500;
    auto wrongCounts = 0;
    for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
    {
        auto source = StopSource {};
        auto calls = std::atomic<int> { 0 };
        auto requester = std::thread { [&source] { static_cast<void>(source.request_stop()); } };
        {
            auto const callback =
                StopCallback<std::function<void()>> { source.get_token(), [&calls] { ++calls; } };
            requester.join();
        }
        wrongCounts += calls.load() == 1 ? 0 : 1;
    }

    CAPTURE(Rounds);
    CHECK(wrongCounts == 0);
}

TEST_CASE("stop_possible stays true while stop is requested and the last source goes", "[StopToken][threads]")
{
    // One thread requests stop and then destroys the last source, while another reads
    // stop_possible() over and over. At every instant a source exists or stop was requested, so
    // every read must be true. Two reads of the state in the wrong order admit a false one: the
    // request and the source's destruction both landing between them.
    constexpr auto Rounds = 200;
    auto roundsWithFalseReads = 0;
    auto readerTimeouts = 0;
    for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
    {
        auto source = std::optional<StopSource> { std::in_place };
        auto const token = source->get_token();
        auto reading = std::atomic<bool> { false };
        auto finished = std::atomic<bool> { false };
        auto readFalse = std::atomic<bool> { false };
        auto reader = std::thread { [&] {
            reading = true;
            while (!finished.load())
            {
                if (!token.stop_possible())
                    readFalse = true;
            }
        } };
        readerTimeouts += waitUntilSet(reading) ? 0 : 1;
        static_cast<void>(source->request_stop());
        source.reset();
        finished = true;
        reader.join();
        roundsWithFalseReads += readFalse.load() ? 1 : 0;
    }

    CAPTURE(Rounds);
    CHECK(readerTimeouts == 0);
    CHECK(roundsWithFalseReads == 0);
}

TEST_CASE("A StopCallback that ran in its constructor does not wait for another at its address",
          "[StopToken][threads]")
{
    // A callback destroys itself while it runs on this thread, and then waits for thread B. B
    // constructs a callback in the same storage, on the stopped token, so it runs in its
    // constructor, and destroys it. That one was never registered, so its destructor has nothing
    // to wait for. Waiting for whatever callback runs at its address would wait for the first
    // one, which waits for B.
    struct Shared
    {
        std::optional<StopCallback<std::function<void()>>> slot;
        std::atomic<bool> firstDestroyed { false };
        std::atomic<bool> secondDone { false };
        std::atomic<bool> firstSawSecondDone { false };
        bool secondSawFirstDestroyed = false; // written by B, read after the join
        bool secondRanInline = false;         // written by B, read after the join
    };
    auto shared = Shared {};
    auto source = StopSource {};
    auto const token = source.get_token();

    shared.slot.emplace(token, [sharedPointer = &shared] {
        auto* const state = sharedPointer; // the closure dies with the callback, on the next line
        state->slot.reset();
        state->firstDestroyed = true;
        state->firstSawSecondDone = waitUntilSet(state->secondDone);
    });

    auto second = std::thread { [&shared, &token] {
        shared.secondSawFirstDestroyed = waitUntilSet(shared.firstDestroyed);
        if (shared.secondSawFirstDestroyed)
        {
            auto ranInline = false;
            shared.slot.emplace(token, [&ranInline] { ranInline = true; });
            shared.secondRanInline = ranInline;
            shared.slot.reset();
        }
        shared.secondDone = true;
    } };
    auto const requested = source.request_stop();
    second.join();

    CHECK(requested);
    CHECK(shared.secondSawFirstDestroyed);
    CHECK(shared.secondRanInline);
    CHECK(shared.firstSawSecondDone.load());
}

#endif
