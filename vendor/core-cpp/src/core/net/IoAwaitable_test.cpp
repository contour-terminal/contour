// SPDX-License-Identifier: Apache-2.0
//
// `ResultAwaitable`'s own contract, driven without a socket, so each rule fails for its own
// reason rather than through whatever a transport does with it:
//
//   - the synchronous-completion path (a decorator with a record already buffered completes from
//     inside the arm hook; `await_suspend` must report "do not suspend" rather than resuming the
//     consumer re-entrantly, which is undefined behaviour and, in a loop, one stack frame per
//     operation);
//   - the three cancellation rules of the design spec's §2 item 5, which are the ones a merge of
//     two libraries gets wrong.
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/IoAwaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <utility>

using core::async::OperationCancelled;
using core::async::StopSource;
using core::async::syncRun;
using core::async::Task;
using core::net::IoAwaitable;
using core::net::IoResult;
using core::net::makeNetError;
using core::net::NetErrorCode;

namespace
{

/// An owner that hands the test the two levers a real transport has: arm, and complete.
///
/// It is not a socket, on purpose — every rule below belongs to the awaitable, and a socket
/// standing in for it would let a socket bug pass for an awaitable bug.
class ScriptedOwner
{
  public:
    /// @return An operation this owner will arm and the test will complete.
    [[nodiscard]] IoAwaitable makeOperation() noexcept
    {
        return IoAwaitable { &ScriptedOwner::arm, &ScriptedOwner::retire, this };
    }

    /// @return An operation whose arm hook records itself at this owner and THEN throws -- the
    ///         two-phase shape that leaks whatever the first phase filed.
    [[nodiscard]] IoAwaitable makeThrowingOperation() noexcept
    {
        return IoAwaitable { &ScriptedOwner::armThenThrow, &ScriptedOwner::retire, this };
    }

    /// @return An operation whose arm hook completes it at once, the way a decorator that already
    ///         has a buffered record does.
    [[nodiscard]] IoAwaitable makeInlineOperation() noexcept
    {
        return IoAwaitable { &ScriptedOwner::armAndComplete, &ScriptedOwner::retire, this };
    }

    /// Completes whatever is armed, exactly as a readiness callback would.
    /// @param result What the operation produced.
    void completeWith(IoResult result) noexcept
    {
        if (auto* const armed = std::exchange(_armed, nullptr); armed != nullptr)
            armed->complete(std::move(result));
    }

    /// Abandons whatever is armed, exactly as a destructor would.
    void abandonIt() noexcept
    {
        if (auto* const armed = std::exchange(_armed, nullptr); armed != nullptr)
            armed->abandon();
    }

    /// What this owner answers an inline completion with.
    std::size_t inlineValue = 0;

    /// @return Whether an operation is parked at this owner right now.
    [[nodiscard]] bool armed() const noexcept { return _armed != nullptr; }

    /// @return How many operations were retired by the awaitable rather than completed.
    [[nodiscard]] int retirements() const noexcept { return _retirements; }

  private:
    static void arm(void* owner, IoAwaitable& self) { static_cast<ScriptedOwner*>(owner)->_armed = &self; }

    static void armThenThrow(void* owner, IoAwaitable& self)
    {
        // Records itself FIRST, exactly as a real owner does -- the slot is claimed and the park is
        // filed before anything that can fail -- and then fails.
        static_cast<ScriptedOwner*>(owner)->_armed = &self;
        throw std::runtime_error { "the loop refused the registration" };
    }

    static void armAndComplete(void* owner, IoAwaitable& self)
    {
        self.complete(IoResult { static_cast<ScriptedOwner*>(owner)->inlineValue });
    }

    static void retire(void* owner, void* awaitable) noexcept
    {
        auto* const me = static_cast<ScriptedOwner*>(owner);
        if (me->_armed != awaitable)
            return; // identity: a retired operation must not be able to retire its successor
        me->_armed = nullptr;
        ++me->_retirements;
    }

    IoAwaitable* _armed = nullptr;
    int _retirements = 0;
};

/// What a flow observed.
enum class Outcome : std::uint8_t
{
    Running,   ///< Still parked.
    Value,     ///< `await_resume` returned a value.
    Error,     ///< `await_resume` returned a NetError as a VALUE.
    Cancelled, ///< `await_resume` threw OperationCancelled.
};

/// Awaits one operation and records how it ended.
/// @param owner Where the operation comes from.
/// @param outcome Where to record what happened.
/// @param bytes Where to record the byte count, for the value case.
/// @param code Where to record the error code, for the error case.
Task<void> awaitOne(ScriptedOwner* owner, Outcome* outcome, std::size_t* bytes, NetErrorCode* code)
{
    try
    {
        auto const result = co_await owner->makeOperation();
        if (result.has_value())
        {
            *bytes = *result;
            *outcome = Outcome::Value;
        }
        else
        {
            *code = result.error().code;
            *outcome = Outcome::Error;
        }
    }
    catch (OperationCancelled const&)
    {
        *outcome = Outcome::Cancelled;
    }
}

/// Awaits an operation whose arm hook throws, and lets the exception out.
Task<void> awaitThrowingArm(ScriptedOwner* owner)
{
    std::ignore = co_await owner->makeThrowingOperation();
}

/// Awaits one inline-completing operation.
Task<std::size_t> awaitInlineOnce(ScriptedOwner* owner)
{
    auto const result = co_await owner->makeInlineOperation();
    co_return result.value_or(0);
}

/// Awaits many inline-completing operations in a loop.
Task<std::size_t> awaitInlineMany(ScriptedOwner* owner, int count)
{
    auto total = std::size_t { 0 };
    for ([[maybe_unused]] auto const iteration: std::views::iota(0, count))
    {
        auto const result = co_await owner->makeInlineOperation();
        total += result.value_or(0);
    }
    co_return total;
}

/// Starts @p task with @p token as its flow token and runs it to its first suspension.
/// @param task The flow; borrowed, so the caller keeps it alive across the park.
/// @param token The flow's cancellation token.
void start(Task<void>& task, core::async::StopToken token)
{
    task.handle().promise().setStopToken(std::move(token));
    task.handle().resume();
}

} // namespace

TEST_CASE("A completion from inside the arm hook resolves the await", "[net][ioawaitable]")
{
    auto owner = ScriptedOwner {};
    owner.inlineValue = 42;
    CHECK(syncRun(awaitInlineOnce(&owner)) == 42);
}

TEST_CASE("Many inline completions in a loop do not recurse to overflow", "[net][ioawaitable]")
{
    // A TLS connection draining pipelined records, each decrypted from an already-buffered BIO so
    // the read completes with no real wait. Resuming the consumer re-entrantly from inside
    // `await_suspend` nests one frame per record; returning the awaiting handle instead iterates
    // in constant stack space. A regression here does not fail the case, it crashes the binary.
    auto owner = ScriptedOwner {};
    owner.inlineValue = 1;
    constexpr auto Iterations = 200000;
    CHECK(syncRun(awaitInlineMany(&owner, Iterations)) == static_cast<std::size_t>(Iterations));
}

TEST_CASE("A cancel from the flow's own token throws", "[net][ioawaitable][cancel]")
{
    // Spec §2 item 5, first rule. The flow is being unwound, so its `co_await` has no sensible
    // value to hand back and the frame must not continue.
    auto owner = ScriptedOwner {};
    auto outcome = Outcome::Running;
    auto bytes = std::size_t { 0 };
    auto code = NetErrorCode::Ok;
    auto source = StopSource {};

    auto flow = awaitOne(&owner, &outcome, &bytes, &code);
    start(flow, source.get_token());
    REQUIRE(owner.armed());
    REQUIRE(outcome == Outcome::Running);

    source.request_stop();
    owner.completeWith(std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "flow cancelled")));
    CHECK(outcome == Outcome::Cancelled);
}

TEST_CASE("A cancel from the RESOURCE is a value, not a throw", "[net][ioawaitable][cancel]")
{
    // Spec §2 item 5, second rule, and the one a merge collapses: the flow is alive and asked a
    // question about a socket that has gone away. Nothing stopped its token, so nothing unwinds.
    auto owner = ScriptedOwner {};
    auto outcome = Outcome::Running;
    auto bytes = std::size_t { 0 };
    auto code = NetErrorCode::Ok;
    auto source = StopSource {};

    auto flow = awaitOne(&owner, &outcome, &bytes, &code);
    start(flow, source.get_token());
    REQUIRE(owner.armed());

    owner.completeWith(std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "socket closed")));
    CHECK(outcome == Outcome::Error);
    CHECK(code == NetErrorCode::Cancelled);
}

TEST_CASE("Bytes that arrived beat a stop the flow has already requested", "[net][ioawaitable][cancel]")
{
    // fastcached#884. A receive that took bytes out of the stream cannot un-take them: they exist
    // nowhere else, so a stop must not discard them.
    //
    // **The stop is requested BEFORE the bytes land, and that ordering is what makes this a test.**
    // The obvious spelling — complete, then stop — asserts nothing at all: completing resumes the
    // flow synchronously, so `await_resume` has already run and returned before the stop exists,
    // and the case passes against an implementation that tests the token first. Measured: it does.
    // This order puts a stopped token and an arrived value in front of `await_resume` at the same
    // instant, which is the state the rule is about.
    //
    // It is also the realistic one. A `whenAny` sibling stops the token while the read is parked,
    // and the loop resolves that cancel on its next turn — but a readiness dispatch in that same
    // turn may already have put seven bytes in the caller's buffer. Those bytes exist nowhere else.
    auto owner = ScriptedOwner {};
    auto outcome = Outcome::Running;
    auto bytes = std::size_t { 0 };
    auto code = NetErrorCode::Ok;
    auto source = StopSource {};

    auto flow = awaitOne(&owner, &outcome, &bytes, &code);
    start(flow, source.get_token());
    REQUIRE(owner.armed());

    source.request_stop();
    REQUIRE(owner.armed()); // no loop routes the cancel here, so the operation is still the owner's
    owner.completeWith(IoResult { std::size_t { 7 } });

    CHECK(outcome == Outcome::Value);
    CHECK(bytes == 7);
}

TEST_CASE("An abandoned operation unwinds whatever the flow's token says", "[net][ioawaitable][cancel]")
{
    // What a socket DESTRUCTOR needs, and the reason `abandon` exists beside `complete`: by the
    // time the flow runs, the socket is gone, so resuming it on its normal path would run its body
    // against storage that no longer exists. The token here is never stopped, so a `complete` would
    // have handed the flow a value and let it carry on.
    auto owner = ScriptedOwner {};
    auto outcome = Outcome::Running;
    auto bytes = std::size_t { 0 };
    auto code = NetErrorCode::Ok;
    auto source = StopSource {};

    auto flow = awaitOne(&owner, &outcome, &bytes, &code);
    start(flow, source.get_token());
    REQUIRE(owner.armed());
    REQUIRE_FALSE(source.get_token().stop_requested());

    owner.abandonIt();
    CHECK(outcome == Outcome::Cancelled);
}

TEST_CASE("A flow cancelled before it parks never arms the operation", "[net][ioawaitable][cancel]")
{
    // Checked before the arm hook runs, so a cancelled flow leaves nothing registered at an owner
    // that nothing would come back to retire.
    auto owner = ScriptedOwner {};
    auto outcome = Outcome::Running;
    auto bytes = std::size_t { 0 };
    auto code = NetErrorCode::Ok;
    auto source = StopSource {};
    source.request_stop();

    auto flow = awaitOne(&owner, &outcome, &bytes, &code);
    start(flow, source.get_token());

    CHECK_FALSE(owner.armed());
    CHECK(owner.retirements() == 0); // nothing was armed, so nothing was retired either
    CHECK(outcome == Outcome::Cancelled);
}

TEST_CASE("An operation destroyed without being awaited retires at its owner", "[net][ioawaitable]")
{
    // `[[nodiscard]]` makes dropping one a warning, which is not a guarantee. An owner claims its
    // slot when the verb is called, so an awaitable that dies unawaited would otherwise leave that
    // slot naming freed storage — and the next readiness would complete into it.
    auto owner = ScriptedOwner {};
    {
        auto const operation = owner.makeOperation();
        CHECK_FALSE(owner.armed()); // nothing is armed until it is awaited
    }
    CHECK(owner.retirements() == 0);

    // Armed, then dropped: this is the shape that has something to retire.
    auto outcome = Outcome::Running;
    auto bytes = std::size_t { 0 };
    auto code = NetErrorCode::Ok;
    auto source = StopSource {};
    {
        auto flow = awaitOne(&owner, &outcome, &bytes, &code);
        start(flow, source.get_token());
        REQUIRE(owner.armed());
        // `flow` is destroyed here with the operation still parked: the frame goes, and with it the
        // awaitable living in it.
    }
    CHECK_FALSE(owner.armed());
    CHECK(owner.retirements() == 1);
}

TEST_CASE("A throw while arming retires the operation instead of leaking it", "[net][ioawaitable][cancel]")
{
    // **The two-phase shape that leaked a park in `InterruptibleSleep`** (core-cpp `8d7b8b2`): the
    // owner records the operation, and only then is the stop callback registered, so an exception
    // out of either half leaves an operation armed with nobody coming back for it. A socket's arm
    // hook can genuinely throw — `registerPark` allocates — and so can the `StopCallback`
    // registration that follows it.
    //
    // It is closed by the DESTRUCTOR rather than by a `try`: `await_suspend` exiting through an
    // exception destroys this awaitable as the awaiting frame unwinds, and `~ResultAwaitable`
    // retires whatever is still armed. This case is what says so, because that reasoning is a
    // property of the language rather than of anything visible in the file.
    // Driven through `syncRun` rather than a bare `resume()`: `Task`'s promise CATCHES the exception
    // into `promise.exception` and rethrows it at `result()`, so `resume()` itself throws nothing. A
    // case that asserted on `resume()` reports "no exception was thrown where one was expected" and
    // reads as though the arm hook never ran. Measured, not reasoned -- that is what it printed.
    auto owner = ScriptedOwner {};
    CHECK_THROWS_AS(syncRun(awaitThrowingArm(&owner)), std::runtime_error);

    CHECK_FALSE(owner.armed());      // the owner was told to forget it
    CHECK(owner.retirements() == 1); // exactly once, and through the retire hook
}
