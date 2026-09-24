// SPDX-License-Identifier: Apache-2.0
#include <core/async/AsTask.hpp>
#include <core/async/Awaitable.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using core::async::asTask;
using core::async::StopSource;
using core::async::syncRun;
using core::async::Task;

namespace
{

/// An awaitable that is ready with a value the moment it is awaited, and counts how many times it
/// was moved — the property `asTask` has to get right is that the awaitable ends up IN the task's
/// frame rather than being awaited through a dangling reference.
class ReadyValue
{
  public:
    explicit ReadyValue(int value) noexcept: _value(value) {}

    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    [[nodiscard]] int await_resume() const noexcept { return _value; }

  private:
    int _value;
};

/// A move-only payload, so the result type cannot be silently copied through the conversion.
using Payload = std::unique_ptr<std::string>;

/// An awaitable resolving to a move-only value.
class ReadyPayload
{
  public:
    explicit ReadyPayload(std::string text): _payload(std::make_unique<std::string>(std::move(text))) {}

    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    [[nodiscard]] Payload await_resume() noexcept { return std::move(_payload); }

  private:
    Payload _payload;
};

/// An awaitable that resolves to nothing, because `void` is the case a `co_return co_await` cannot
/// express and therefore the one a conversion gets wrong.
class ReadyVoid
{
  public:
    explicit ReadyVoid(int* ran) noexcept: _ran(ran) {}

    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept { ++*_ran; }

  private:
    int* _ran;
};

/// An awaitable that throws from `await_resume`, which is how every failing socket operation in
/// `core::net` reports a cancelled flow.
class ThrowingAwaitable
{
  public:
    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    [[noreturn]] int await_resume() const { throw std::runtime_error { "awaitable failed" }; }
};

/// An awaitable that reads the awaiting promise's stop token, which is what makes a socket
/// operation stop-aware. It records what it saw so a case can assert the token SURVIVED the
/// conversion rather than being replaced by the freshly-made task's own.
class TokenProbe
{
  public:
    explicit TokenProbe(bool* sawStop) noexcept: _sawStop(sawStop) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
    {
        if constexpr (core::async::HasStopToken<Promise>)
            *_sawStop = awaiting.promise().stopToken().stop_requested();
        return false; // never actually parks; the point is which promise it was handed
    }

    void await_resume() const noexcept {}

  private:
    bool* _sawStop;
};

/// An awaitable that counts how many of itself exist, so a case can ask whether the one handed to
/// `asTask` is still alive after the full-expression that made it ended.
class Counted
{
  public:
    explicit Counted(int value) noexcept: _value(value) { ++LiveInstances; }
    Counted(Counted const& other) noexcept: _value(other._value) { ++LiveInstances; }
    Counted(Counted&& other) noexcept: _value(other._value) { ++LiveInstances; }
    Counted& operator=(Counted const&) = delete;
    Counted& operator=(Counted&&) = delete;
    ~Counted() { --LiveInstances; }

    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    [[nodiscard]] int await_resume() const noexcept { return _value; }

    /// @return How many instances exist right now.
    [[nodiscard]] static int liveCount() noexcept { return LiveInstances; }

  private:
    /// CamelCase with no underscore: a STATIC data member is a class member under this tree's
    /// naming rule, not a private instance member, so it does not take the `_` prefix.
    static int LiveInstances;
    int _value;
};

int Counted::LiveInstances = 0;

/// Awaits the probe through a converted task, so the token has two links to cross.
/// @param sawStop Where the probe records what token it was handed.
Task<void> awaitThroughATask(bool* sawStop)
{
    co_await asTask(TokenProbe { sawStop });
}

} // namespace

TEST_CASE("asTask carries an awaitable's value out as a Task", "[async][astask]")
{
    auto task = asTask(ReadyValue { 7 });
    static_assert(std::is_same_v<decltype(task), Task<int>>,
                  "asTask must deduce the awaitable's await_resume type");
    auto const value = syncRun(std::move(task));
    CHECK(value == 7);
}

TEST_CASE("asTask moves a move-only result out rather than copying it", "[async][astask]")
{
    auto const payload = syncRun(asTask(ReadyPayload { "frame-free" }));
    REQUIRE(payload != nullptr);
    CHECK(*payload == "frame-free");
}

TEST_CASE("asTask converts an awaitable that resolves to nothing", "[async][astask]")
{
    auto ran = 0;
    auto task = asTask(ReadyVoid { &ran });
    static_assert(std::is_same_v<decltype(task), Task<void>>,
                  "a void awaitable must become a Task<void>, not a Task of something");
    syncRun(std::move(task));
    CHECK(ran == 1);
}

TEST_CASE("asTask propagates the exception an awaitable resumes with", "[async][astask]")
{
    CHECK_THROWS_AS(syncRun(asTask(ThrowingAwaitable {})), std::runtime_error);
}

TEST_CASE("asTask hands the awaitable the AWAITING flow's stop token", "[async][astask]")
{
    // The whole reason a socket operation is an awaitable rather than a task: it reads the token of
    // the coroutine that awaits it. A conversion that dropped it would leave the operation
    // un-cancellable, and the symptom is a read that never unwinds.
    //
    // The token is set on the OUTER task, two links up the chain from the probe, so what this
    // asserts is propagation THROUGH the conversion rather than into it.
    auto sawStop = false;
    auto outer = awaitThroughATask(&sawStop);
    auto source = StopSource {};
    source.request_stop();
    outer.handle().promise().setStopToken(source.get_token());
    syncRun(std::move(outer));
    CHECK(sawStop);
}

TEST_CASE("asTask stores the awaitable, so a temporary's lifetime is the task's", "[async][astask]")
{
    // Asserting the VALUE after the temporary died would not discriminate: freed stack usually
    // still reads 99. What discriminates is whether the awaitable is still ALIVE once the
    // full-expression that made it has ended — one instance if the frame owns it, zero if asTask
    // merely bound a reference to the temporary.
    REQUIRE(Counted::liveCount() == 0);
    auto task = asTask(Counted { 99 });
    CHECK(Counted::liveCount() == 1);
    auto const value = syncRun(std::move(task));
    CHECK(value == 99);
    CHECK(Counted::liveCount() == 0);
}
