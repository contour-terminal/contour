// SPDX-License-Identifier: Apache-2.0
#include <core/async/AsyncQueue.hpp>
#include <core/async/Awaitable.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/KeyedStrands.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/StrandTestSupport.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #include <core/async/ThreadPoolExecutor.hpp>

    #include <array>
    #include <chrono>
    #include <mutex>
    #include <ranges>
    #include <thread>
#endif

using core::async::currentExecutor;
using core::async::DetachedTask;
using core::async::IExecutor;
using core::async::ParkedWork;
using core::async::Task;
using core::async::testing::ManualExecutor;

namespace
{

using Strands = core::async::KeyedStrands<int>;
using Queue = core::async::AsyncQueue<int>;

static_assert(core::async::awaitReadyIsConstantFalse<Strands::ResumeOnKey>());

/// What a task saw of the strands while it ran.
struct Look
{
    bool onOne { false };   ///< `runningHere(1)`.
    bool onTwo { false };   ///< `runningHere(2)`.
    bool onAny { false };   ///< `runningAnyHere()`.
    bool current { false }; ///< Whether any executor was current.
};

/// Hops onto @p key's strand and records what it sees there.
Task<void> lookFrom(Strands* strands, int key, std::vector<Look>* out)
{
    co_await strands->resumeOn(key);
    out->push_back(Look { .onOne = strands->runningHere(1),
                          .onTwo = strands->runningHere(2),
                          .onAny = strands->runningAnyHere(),
                          .current = currentExecutor() != nullptr });
}

/// Hops onto @p key's strand, records its value, and ends.
Task<void> append(Strands* strands, int key, std::vector<int>* out, int value)
{
    co_await strands->resumeOn(key);
    out->push_back(value);
}

/// A frame sentinel, counted when the frame carrying it dies.
class FrameSentinel
{
  public:
    explicit FrameSentinel(int* destroyed) noexcept: _destroyed(destroyed) {}
    FrameSentinel(FrameSentinel&& other) noexcept: _destroyed(std::exchange(other._destroyed, nullptr)) {}
    FrameSentinel(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel&&) = delete;

    ~FrameSentinel()
    {
        if (_destroyed != nullptr)
            ++*_destroyed;
    }

  private:
    int* _destroyed;
};

/// What a keyed consumer saw.
struct Consumed
{
    std::vector<int> seen;   ///< Every value it took.
    std::vector<bool> onKey; ///< For each value, whether it was on its key's strand.
    bool closed { false };   ///< Whether it saw the queue close.
};

/// Hops onto @p key's strand, then pops until the queue closes.
Task<void> consumeOnKey(Strands* strands, int key, Queue* queue, Consumed* out)
{
    co_await strands->resumeOn(key);
    while (auto item = co_await queue->pop())
    {
        out->seen.push_back(*item);
        out->onKey.push_back(strands->runningHere(key));
    }
    out->closed = true;
}

} // namespace

TEST_CASE("KeyedStrands answers runningHere per key and runningAnyHere for all of them", "[KeyedStrands]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    auto looks = std::vector<Look> {};

    auto one = lookFrom(&strands, 1, &looks);
    auto two = lookFrom(&strands, 2, &looks);
    one.handle().resume();
    two.handle().resume();
    std::ignore = base.drain();

    REQUIRE(looks.size() == 2);
    CHECK(looks[0].onOne);
    CHECK_FALSE(looks[0].onTwo);
    CHECK(looks[0].onAny);
    CHECK(looks[0].current);
    CHECK_FALSE(looks[1].onOne);
    CHECK(looks[1].onTwo);
    CHECK(looks[1].onAny);

    // And nothing, outside.
    CHECK_FALSE(strands.runningHere(1));
    CHECK_FALSE(strands.runningAnyHere());
}

TEST_CASE("KeyedStrands runs one key's work in FIFO order", "[KeyedStrands]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    auto order = std::vector<int> {};

    auto tasks = std::vector<Task<void>> {};
    for (auto const value: { 0, 1, 2, 3, 4, 5 })
        tasks.push_back(append(&strands, 7, &order, value));
    for (auto& task: tasks)
        task.handle().resume();
    std::ignore = base.drain();
    CHECK(order == std::vector { 0, 1, 2, 3, 4, 5 });
}

TEST_CASE("An idle key's strand is reclaimed, and a later submit makes a new one", "[KeyedStrands]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    auto order = std::vector<int> {};
    CHECK(strands.size() == 0);

    auto first = append(&strands, 1, &order, 10);
    auto second = append(&strands, 2, &order, 20);
    auto third = append(&strands, 3, &order, 30);
    first.handle().resume();
    second.handle().resume();
    third.handle().resume();
    CHECK(strands.size() == 3);

    std::ignore = base.drain();
    CHECK(order.size() == 3);
    // Every key ran out of work, so every strand is gone: one per key ever seen would grow without
    // bound in a program keyed by connection or by model instance.
    CHECK(strands.size() == 0);

    auto again = append(&strands, 1, &order, 11);
    again.handle().resume();
    CHECK(strands.size() == 1);
    std::ignore = base.drain();
    CHECK(order.back() == 11);
    CHECK(strands.size() == 0);
}

TEST_CASE("A coroutine that parked while its key's strand was reclaimed comes back to that key",
          "[KeyedStrands][AsyncQueue][context]")
{
    // The resume target a parked coroutine holds names a strand that went idle and was reclaimed
    // while it waited. It must neither dangle nor come back on a strand of its own beside the key's
    // new one: it comes back to the key.
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto strands = Strands { base };
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto out = Consumed {};
    auto order = std::vector<int> {};

    auto consumer = consumeOnKey(&strands, 5, &queue, &out);
    consumer.handle().resume();
    std::ignore = base.drain();
    REQUIRE(queue.hasWaiter());
    CHECK(strands.size() == 0);

    // Another task on the same key, queued but not run, so the key has a live strand again.
    auto other = append(&strands, 5, &order, 1);
    other.handle().resume();
    CHECK(strands.size() == 1);

    std::ignore = queue.push(42);
    CHECK(foreign.pending() == 0);
    // Still one strand for the key: the resumption joined it rather than making a second.
    CHECK(strands.size() == 1);
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    CHECK(order == std::vector { 1 });
    REQUIRE(out.seen == std::vector { 42 });
    CHECK(out.onKey == std::vector { true });

    queue.close();
    std::ignore = base.drain();
    CHECK(out.closed);
    CHECK(strands.size() == 0);
}

TEST_CASE("KeyedStrands destroyed with work queued drops it, freeing what nobody owns", "[KeyedStrands]")
{
    auto base = ManualExecutor {};
    auto ran = 0;
    auto ended = 0;
    {
        auto strands = Strands { base };
        [](Strands* on, int* count, FrameSentinel sentinel) -> DetachedTask {
            (void) sentinel;
            co_await on->resumeOn(3);
            ++*count;
        }(&strands, &ran, FrameSentinel { &ended });
        CHECK(strands.size() == 1);
        CHECK(ended == 0);
    }
    // The detached chain was freed with the strands, never run.
    CHECK(ended == 1);
    CHECK(ran == 0);
    std::ignore = base.drain();
    CHECK(ran == 0);
}

namespace
{

/// An object that owns keyed strands, released from inside one of their tasks.
class KeyedOwner
{
  public:
    explicit KeyedOwner(IExecutor& base): _strands(base) {}

    /// @return The strands this owner's work runs on.
    [[nodiscard]] Strands& strands() noexcept { return _strands; }

  private:
    Strands _strands;
};

/// Hops onto key 1 and, from inside that task, releases the last reference to the owner.
Task<void> releaseKeyedOwner(std::shared_ptr<KeyedOwner>* holder, std::atomic<bool>* released)
{
    co_await (*holder)->strands().resumeOn(1);
    holder->reset();
    released->store(true);
}

} // namespace

TEST_CASE("KeyedStrands destroyed from inside one of its own tasks neither waits for itself nor runs "
          "what is queued",
          "[KeyedStrands][lifetime]")
{
    auto base = ManualExecutor {};
    auto released = std::atomic<bool> { false };
    auto order = std::vector<int> {};
    auto holder = std::make_shared<KeyedOwner>(base);

    auto releaser = releaseKeyedOwner(&holder, &released);
    auto sameKey = append(&holder->strands(), 1, &order, 1);
    auto otherKey = append(&holder->strands(), 2, &order, 2);
    releaser.handle().resume();
    sameKey.handle().resume();
    otherKey.handle().resume();
    std::ignore = base.drain();

    CHECK(released.load());
    CHECK(releaser.done());
    CHECK(holder == nullptr);
    // Key 2's task could have run before key 1's release, since the keys are independent: which
    // one the base takes first is its business. What cannot happen is either running AFTER.
    CHECK((order.empty() || order == std::vector { 2 }));
    CHECK_FALSE(sameKey.done());
    CHECK(base.pending() == 0);
}

namespace
{

/// A base that refuses every submit.
class RefusingBase final: public IExecutor
{
  public:
    using IExecutor::submit;
    void submit(std::coroutine_handle<> /*handle*/) override { throw std::runtime_error { "refused" }; }
    void submit(ParkedWork /*work*/) override { throw std::runtime_error { "refused" }; }
};

} // namespace

TEST_CASE("A key whose first hand-off the base refuses leaves no strand behind", "[KeyedStrands][exceptions]")
{
    // Before the fix the key's new strand stayed in the registry, idle, with nothing to retire it:
    // size() stayed 1 and waitIdle() never returned.
    auto base = RefusingBase {};
    auto strands = Strands { base };
    auto order = std::vector<int> {};
    auto task = append(&strands, 9, &order, 1);

    task.handle().resume(); // the refusal reaches the Task, which keeps it for its awaiter
    REQUIRE(task.done());
    CHECK_THROWS_AS(task.result(), std::runtime_error);
    CHECK(strands.size() == 0);
    CHECK(order.empty());
}

#if !defined(_MSC_VER) || defined(__clang__)
namespace
{

/// A base that refuses the next @c refuse submits, and otherwise queues like @c ManualExecutor.
class RefusingExecutor final: public IExecutor
{
  public:
    using IExecutor::submit;

    /// Refuses the next @p count submits.
    /// @param count How many.
    void refuse(int count) noexcept { _refuse = count; }

    void submit(std::coroutine_handle<> handle) override { submit(ParkedWork { .resume = handle }); }

    void submit(ParkedWork work) override
    {
        if (_refuse > 0)
        {
            --_refuse;
            throw std::runtime_error { "refused" };
        }
        _inner.submit(std::move(work));
    }

    /// @return How many entries were resumed.
    std::size_t drain() { return _inner.drain(); }

    /// @return How many entries are queued.
    [[nodiscard]] std::size_t pending() const { return _inner.pending(); }

  private:
    ManualExecutor _inner;
    int _refuse { 0 };
};

/// A coroutine whose `resume()` throws -- see `Strand_test.cpp`.
class ThrowingResume
{
  public:
    struct promise_type
    {
        [[nodiscard]] ThrowingResume get_return_object() noexcept
        {
            return ThrowingResume { std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        [[noreturn]] void unhandled_exception() const { throw; }
    };

    explicit ThrowingResume(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
    ThrowingResume(ThrowingResume const&) = delete;
    ThrowingResume(ThrowingResume&&) = delete;
    ThrowingResume& operator=(ThrowingResume const&) = delete;
    ThrowingResume& operator=(ThrowingResume&&) = delete;
    ~ThrowingResume() { _handle.destroy(); }

    [[nodiscard]] std::coroutine_handle<> handle() const noexcept { return _handle; }

  private:
    std::coroutine_handle<promise_type> _handle;
};

/// Throws out of `resume()`.
ThrowingResume throwOnResume(bool really)
{
    if (really)
        throw std::logic_error { "a task that throws out of resume()" };
    co_return;
}

/// What a @c ResubmitWhenFreed saw.
struct Resubmits
{
    int freed { 0 };      ///< Times a frame carrying one was freed.
    bool threw { false }; ///< Whether the submit from its destructor threw.
};

/// Submits to one key from its destructor, recording rather than letting out what that throws.
class ResubmitWhenFreed
{
  public:
    ResubmitWhenFreed(Strands* strands, int key, Resubmits* out) noexcept:
        _strands(strands), _key(key), _out(out)
    {
    }
    ResubmitWhenFreed(ResubmitWhenFreed&& other) noexcept:
        _strands(std::exchange(other._strands, nullptr)), _key(other._key), _out(other._out)
    {
    }
    ResubmitWhenFreed(ResubmitWhenFreed const&) = delete;
    ResubmitWhenFreed& operator=(ResubmitWhenFreed const&) = delete;
    ResubmitWhenFreed& operator=(ResubmitWhenFreed&&) = delete;

    ~ResubmitWhenFreed()
    {
        if (_strands == nullptr)
            return;
        ++_out->freed;
        try
        {
            _strands->submit(_key, std::noop_coroutine());
        }
        catch (...)
        {
            _out->threw = true;
        }
    }

  private:
    Strands* _strands;
    int _key;
    Resubmits* _out;
};

/// A detached flow queued on @p key, carrying a @c ResubmitWhenFreed.
DetachedTask resubmitsWhenFreed(Strands* strands, int key, ResubmitWhenFreed guard, int* ran)
{
    (void) guard;
    co_await strands->resumeOn(key);
    ++*ran;
}

} // namespace
#endif

TEST_CASE("Work a key's refused hand-off frees is dropped, not refused, when it submits to the key again",
          "[KeyedStrands][exceptions]")
{
#if defined(_MSC_VER) && !defined(__clang__)
    SKIP("under MSVC's cl a throw out of resume() on a strand terminates the process "
         "(core-cpp.strand-throw-canary)");
#else
    // As for a Strand: the destructor's submit found the key's strand still registered and open,
    // queued its pump on the refusing base, and threw out of a destructor.
    auto base = RefusingExecutor {};
    auto strands = Strands { base };
    auto resubmits = Resubmits {};
    auto ran = 0;

    auto const thrower = throwOnResume(true);
    strands.submit(1, thrower.handle());
    resubmitsWhenFreed(&strands, 1, ResubmitWhenFreed { &strands, 1, &resubmits }, &ran);

    base.refuse(2);
    // What leaves is the refusal of the replacement pump, not the task's own exception.
    auto thrown = std::string {};
    try
    {
        std::ignore = base.drain();
    }
    catch (std::exception const& error)
    {
        thrown = error.what();
    }
    CHECK(thrown == "refused");
    base.refuse(0);
    CHECK(resubmits.freed == 1);
    CHECK_FALSE(resubmits.threw);
    CHECK(ran == 0);
    CHECK(strands.size() == 0);
    CHECK(base.pending() == 0);

    // The key still takes work.
    auto order = std::vector<int> {};
    auto after = append(&strands, 1, &order, 7);
    after.handle().resume();
    std::ignore = base.drain();
    CHECK(order == std::vector { 7 });
#endif
}

namespace
{

/// The session a keyed hook installs for the length of one task.
thread_local int keyedSession = 0;

/// Installs a session derived from the key -- as morph installs the session of the action its
/// model instance is running -- and records which keys it was called for.
struct KeySessionHook
{
    std::vector<int> keys;

    void operator()(int const& key, core::async::RunTask run)
    {
        keys.push_back(key);
        auto const previous = std::exchange(keyedSession, 100 * key);
        run();
        keyedSession = previous;
    }
};

/// Hops onto @p key's strand, then pops once, recording the session at each resumption.
Task<void> keyedSessionConsumer(Strands* strands, int key, Queue* queue, std::vector<int>* sessions)
{
    co_await strands->resumeOn(key);
    sessions->push_back(keyedSession);
    std::ignore = co_await queue->pop();
    sessions->push_back(keyedSession);
}

/// A callable that owns something, so a test can see whether it was moved from.
struct OwningCall
{
    std::unique_ptr<int> payload;
    std::vector<int>* out;

    void operator()() const { out->push_back(*payload); }
};

} // namespace

TEST_CASE("KeyedStrands::post runs a callable on its key's strand, in order with that key's other work",
          "[KeyedStrands][post]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    auto order = std::vector<int> {};
    auto onKey = std::vector<bool> {};

    strands.post(1, [&order, &onKey, &strands] {
        order.push_back(1);
        onKey.push_back(strands.runningHere(1) && !strands.runningHere(2));
    });
    auto second = append(&strands, 1, &order, 2);
    second.handle().resume();
    strands.post(1, [&order] { order.push_back(3); });
    strands.post(2, [&order, &onKey, &strands] {
        order.push_back(20);
        onKey.push_back(strands.runningHere(2));
    });
    CHECK(strands.size() == 2);

    std::ignore = base.drain();
    // Key 1's three in order; key 2's anywhere among them.
    auto keyOne = std::vector<int> {};
    std::ranges::copy_if(order, std::back_inserter(keyOne), [](int value) { return value < 10; });
    CHECK(keyOne == std::vector { 1, 2, 3 });
    CHECK(std::ranges::count(order, 20) == 1);
    CHECK(onKey == std::vector { true, true });
    CHECK(strands.size() == 0);
}

TEST_CASE("KeyedStrands' tryPost and trySubmit leave the work with the caller once they are closed",
          "[KeyedStrands][post]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    auto order = std::vector<int> {};

    auto first = OwningCall { .payload = std::make_unique<int>(1), .out = &order };
    CHECK(strands.tryPost(4, first));
    std::ignore = base.drain();
    CHECK(order == std::vector { 1 });

    strands.close();
    strands.close(); // idempotent

    auto call = OwningCall { .payload = std::make_unique<int>(2), .out = &order };
    CHECK_FALSE(strands.tryPost(4, call));
    CHECK(call.payload != nullptr);

    auto refused = append(&strands, 4, &order, 3); // not started: its handle is only borrowed below
    auto work = ParkedWork { .resume = refused.handle() };
    CHECK_FALSE(strands.trySubmit(4, work));
    CHECK(work.resume == refused.handle());
    CHECK_FALSE(strands.trySubmit(4, refused.handle()));

    strands.post(4, [&order] { order.push_back(5); });
    CHECK(base.pending() == 0);
    CHECK(order == std::vector { 1 });
    CHECK(strands.size() == 0);
}

namespace
{

/// Hops onto @p key's strand, records whether it is there, parks on @p queue, and records again.
Task<void> parkOnKey(Strands* strands, int key, Queue* queue, std::vector<bool>* onKey)
{
    co_await strands->resumeOn(key);
    onKey->push_back(strands->runningHere(key));
    std::ignore = co_await queue->pop();
    onKey->push_back(strands->runningHere(key));
}

} // namespace

TEST_CASE("A retired strand that a parked coroutine still holds is not given to another key",
          "[KeyedStrands][lifetime]")
{
    // Retired strands are kept for reuse. One that a coroutine parked on it still references must
    // keep serving its own key -- handing the coroutine back to that key's strand -- and never be
    // given to another: the coroutine would come back on the wrong key.
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto strands = Strands { base };
    auto onKey = std::vector<bool> {};
    auto onOther = std::vector<bool> {};

    auto parked = parkOnKey(&strands, 1, &queue, &onKey);
    parked.handle().resume();
    std::ignore = base.drain(); // key 1's task parks; its strand retires
    REQUIRE(queue.hasWaiter());
    REQUIRE(strands.size() == 0);

    strands.post(2, [&onOther, &strands] {
        onOther.push_back(strands.runningHere(2));
        onOther.push_back(strands.runningHere(1));
    });
    std::ignore = base.drain();
    std::ignore = queue.push(1); // back to key 1
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    CHECK(parked.done());
    CHECK(onKey == std::vector { true, true });
    CHECK(onOther == std::vector { true, false });
    CHECK(strands.size() == 0);
}

TEST_CASE("KeyedStrands::idle is true once no key has work queued or running", "[KeyedStrands][idle]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    CHECK(strands.idle());
    strands.post(1, [] {});
    strands.post(2, [] {});
    CHECK_FALSE(strands.idle());
    // A host-driven loop's teardown: pump the base until the strands are idle.
    while (!strands.idle() && base.runOne())
    {
    }
    CHECK(strands.idle());
    CHECK(strands.size() == 0);
}

TEST_CASE("A keyed around-task hook is given the key, around every resumption on that key's strand",
          "[KeyedStrands][aroundTask]")
{
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto hook = KeySessionHook {};
    auto strands =
        Strands { base, core::async::StrandOptions {}, core::async::KeyedAroundTask<int>::of(hook) };
    auto sessions = std::vector<int> {};

    auto consumer = keyedSessionConsumer(&strands, 3, &queue, &sessions);
    consumer.handle().resume();
    std::ignore = base.drain();
    REQUIRE(queue.hasWaiter());
    std::ignore = queue.push(1); // back through key 3's strand -- a new one, the old one retired
    std::ignore = base.drain();
    std::ignore = foreign.drain();
    strands.post(5, [] { CHECK(keyedSession == 500); });
    std::ignore = base.drain();

    CHECK(consumer.done());
    CHECK(sessions == std::vector { 300, 300 });
    CHECK(hook.keys == std::vector { 3, 3, 5 });
    CHECK(keyedSession == 0);
}

namespace
{

using core::async::RunTask;
using core::async::TaskKind;

/// A keyed hook that records each task's key and kind, and runs it.
struct KeyedKindHook
{
    std::vector<std::pair<int, TaskKind>> seen;

    void operator()(int const& key, RunTask run)
    {
        seen.emplace_back(key, run.kind());
        run();
    }
};

/// Counts itself run, and ends: a coroutine that is one task wherever it is submitted.
Task<void> countOnce(int* ran)
{
    ++*ran;
    co_return;
}

/// Hops onto @p key's strand, parks on @p queue, and counts each time it runs on the key.
Task<void> parkOnQueue(Strands* strands, int key, Queue* queue, int* ran)
{
    co_await strands->resumeOn(key);
    ++*ran;
    std::ignore = co_await queue->pop();
    ++*ran;
}

} // namespace

TEST_CASE("A keyed around-task hook is told whether it runs a posted callable or a coroutine resumption",
          "[KeyedStrands][aroundTask][kind]")
{
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    auto hook = KeyedKindHook {};
    auto strands =
        Strands { base, core::async::StrandOptions {}, core::async::KeyedAroundTask<int>::of(hook) };
    auto order = std::vector<int> {};

    strands.post(1, [&order] { order.push_back(1); });
    auto call = OwningCall { .payload = std::make_unique<int>(2), .out = &order };
    CHECK(strands.tryPost(2, call));
    std::ignore = base.drain();

    auto counted = 0;
    auto byHandle = countOnce(&counted);
    strands.submit(4, byHandle.handle());
    auto destroyed = 0;
    auto root = std::coroutine_handle<> {};
    core::async::test::parkDetached(&root, FrameSentinel { &destroyed });
    REQUIRE(root);
    strands.submit(5, ParkedWork { .resume = root, .abandon = core::async::detail::claimOn(root) });
    std::ignore = base.drain();

    // A resumption rerouted through a retired key strand: the coroutine parks off its key, the key's
    // strand retires, and the push comes back through the retired strand to the registry.
    auto ran = 0;
    auto parked = parkOnQueue(&strands, 6, &queue, &ran);
    parked.handle().resume(); // resumeOn(6): a hop
    std::ignore = base.drain();
    REQUIRE(queue.hasWaiter());
    REQUIRE(strands.size() == 0); // key 6's strand retired under the parked coroutine
    std::ignore = queue.push(1);
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    using Seen = std::pair<int, TaskKind>;
    CHECK(hook.seen
          == std::vector { Seen { 1, TaskKind::Callable },
                           Seen { 2, TaskKind::Callable },
                           Seen { 4, TaskKind::Resumption },
                           Seen { 5, TaskKind::Resumption },
                           Seen { 6, TaskKind::Resumption },
                           Seen { 6, TaskKind::Resumption } });
    CHECK(order == std::vector { 1, 2 });
    CHECK(counted == 1);
    CHECK(byHandle.done());
    CHECK(destroyed == 1);
    CHECK(ran == 2);
    CHECK(parked.done());
}

TEST_CASE("KeyedStrands::seal() closes the offer door for every key, and runs what is queued and what "
          "comes back",
          "[KeyedStrands][seal]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    auto order = std::vector<int> {};

    strands.post(1, [&order] { order.push_back(1); });
    auto queued = append(&strands, 2, &order, 2);
    queued.handle().resume(); // queued on key 2
    strands.seal();
    strands.seal(); // idempotent
    CHECK(strands.size() == 2);

    auto refusedCall = OwningCall { .payload = std::make_unique<int>(3), .out = &order };
    CHECK_FALSE(strands.tryPost(1, refusedCall)); // a key with queued work
    CHECK_FALSE(strands.tryPost(7, refusedCall)); // a key with none: no strand is made for it
    CHECK(refusedCall.payload != nullptr);
    CHECK(strands.size() == 2);
    auto refused = append(&strands, 7, &order, 4);
    auto work = ParkedWork { .resume = refused.handle() };
    CHECK_FALSE(strands.trySubmit(7, work));
    CHECK(work.resume == refused.handle());

    // post and submit are how admitted work comes back, and are admitted until close() -- a key with
    // no strand included.
    strands.post(1, [&order] { order.push_back(5); });
    auto cameBack = append(&strands, 9, &order, 6);
    cameBack.handle().resume();
    CHECK(strands.size() == 3);

    while (!strands.idle() && base.runOne())
    {
    }
    CHECK(strands.idle());
    CHECK(strands.size() == 0);
    std::ranges::sort(order);
    CHECK(order == std::vector { 1, 2, 5, 6 });
    CHECK(queued.done());
    CHECK(cameBack.done());
    CHECK_FALSE(refused.done());
    CHECK_FALSE(strands.tryPost(8, refusedCall)); // the seal holds for good
    strands.close();
}

TEST_CASE("A coroutine parked off its key's strand when the strands are sealed comes back to its key",
          "[KeyedStrands][seal][AsyncQueue]")
{
    // As for a Strand, both ways back: once while the key's strand is still alive -- work queued
    // behind the parked coroutine keeps it from retiring -- so the push submits to that strand
    // directly, and once after it retired, so it reroutes through the registry.
    auto base = ManualExecutor {};
    auto foreign = ManualExecutor {};
    auto queue = Queue { foreign, core::async::AsyncQueueOptions {} };
    // One task per turn, so work queued behind the consumer is still queued when it has parked.
    auto strands = Strands { base, core::async::StrandOptions { .batch = 1 } };
    auto out = Consumed {};
    auto consumer = consumeOnKey(&strands, 3, &queue, &out);
    consumer.handle().resume();
    auto behind = false;
    strands.post(3, [&behind] { behind = true; }); // keeps key 3's strand alive past the park
    std::ignore = base.runOne();                   // the consumer's turn: it runs to the pop, and parks
    REQUIRE(queue.hasWaiter());
    REQUIRE(strands.size() == 1);
    REQUIRE_FALSE(behind);

    strands.seal();
    std::ignore = queue.push(1); // straight to key 3's live strand
    std::ignore = base.drain();
    REQUIRE(queue.hasWaiter());
    CHECK(strands.idle());       // key 3's strand retired: the parked coroutine is invisible to idle()
    std::ignore = queue.push(2); // through the retired strand's reroute to the registry
    std::ignore = base.drain();
    queue.close();
    std::ignore = base.drain();
    std::ignore = foreign.drain();

    CHECK(behind);
    CHECK(out.seen == std::vector { 1, 2 });
    CHECK(out.onKey == std::vector { true, true });
    CHECK(out.closed);
    CHECK(consumer.done());
    CHECK(strands.size() == 0);
    strands.close();
}

TEST_CASE("A trySubmit sealed KeyedStrands refuse leaves the caller an armed claim", "[KeyedStrands][seal]")
{
    auto base = ManualExecutor {};
    auto strands = Strands { base };
    auto destroyed = 0;
    auto root = std::coroutine_handle<> {};
    core::async::test::parkDetached(&root, FrameSentinel { &destroyed });
    REQUIRE(root);
    auto work = ParkedWork { .resume = root, .abandon = core::async::detail::claimOn(root) };
    REQUIRE(work.abandon.armed());

    strands.seal();
    CHECK_FALSE(strands.trySubmit(4, work));
    CHECK(work.resume == root);
    CHECK(work.abandon.armed());
    CHECK(destroyed == 0);
    work.abandon.reset();
    CHECK(destroyed == 1);
}

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)

namespace
{

using namespace std::chrono_literals;

/// How long a case waits for other threads before it calls the machine wedged.
constexpr auto Budget = 60s;

/// Polls @p done on a monotonic clock until it holds or the budget runs out.
/// @return Whether it held.
template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate done)
{
    auto const deadline = std::chrono::steady_clock::now() + Budget;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// Calls `waitIdle()` on a thread of its own and waits a bounded time for it to return. Where it
/// does not, it says so -- how long, and how many keys still had a strand -- and closes the strands,
/// which ends the wait, so the case fails rather than hangs.
/// @param strands The strands.
/// @return Whether `waitIdle()` returned on its own within the budget.
[[nodiscard]] bool waitIdleWithin(Strands& strands)
{
    auto returned = std::atomic<bool> { false };
    auto waiter = std::thread { [&strands, &returned] {
        strands.waitIdle();
        returned.store(true);
    } };
    auto const inTime = waitUntil([&returned] { return returned.load(); });
    if (!inTime)
    {
        UNSCOPED_INFO("waitIdle() had not returned after " << Budget.count() << " s; " << strands.size()
                                                           << " key(s) still had a strand");
        strands.close(); // wakes the wait
    }
    waiter.join();
    return inTime;
}

/// Per-key overlap accounting for the concurrency cases.
struct PerKey
{
    std::array<std::atomic<int>, 4> inside {}; ///< Tasks of each key inside right now.
    std::atomic<int> overlaps { 0 };           ///< Times a task found its own key busy.
    std::atomic<int> maxDistinct { 0 };        ///< Most keys seen inside at once.
    std::atomic<int> busyKeys { 0 };           ///< Keys with a task inside right now.
    std::atomic<int> finished { 0 };           ///< Tasks that have ended.
};

/// Hops onto @p key's strand and holds it for a while, counting company of the same key and of
/// the others.
DetachedTask keyedProbe(Strands* strands, PerKey* probe, int key)
{
    co_await strands->resumeOn(key);
    auto& mine = probe->inside.at(static_cast<std::size_t>(key));
    if (mine.fetch_add(1) != 0)
        probe->overlaps.fetch_add(1);
    auto const busy = probe->busyKeys.fetch_add(1) + 1;
    auto seen = probe->maxDistinct.load();
    while (busy > seen && !probe->maxDistinct.compare_exchange_weak(seen, busy))
    {
    }
    std::this_thread::sleep_for(200us);
    probe->busyKeys.fetch_sub(1);
    mine.fetch_sub(1);
    probe->finished.fetch_add(1);
}

/// The same accounting as @c keyedProbe, for a task that holds its strand for no time at all, so
/// that its key goes idle -- and is retired -- as often as a producer lets it.
/// @param strands The strands. @param probe The accounting. @param key The key.
void keyedTick(Strands* strands, PerKey* probe, int key)
{
    strands->post(key, [probe, key] {
        auto& mine = probe->inside.at(static_cast<std::size_t>(key));
        if (mine.fetch_add(1) != 0)
            probe->overlaps.fetch_add(1);
        mine.fetch_sub(1);
        probe->finished.fetch_add(1);
    });
}

} // namespace

TEST_CASE("Different keys run concurrently and the same key never overlaps", "[KeyedStrands][threads]")
{
    constexpr auto PerKeyCount = 200;
    auto probe = PerKey {};
    auto pool = core::async::ThreadPoolExecutor { 4 };
    {
        auto strands = Strands { pool };
        for ([[maybe_unused]] auto const round: std::views::iota(0, PerKeyCount))
            for (auto const key: { 0, 1, 2, 3 })
                keyedProbe(&strands, &probe, key);
        auto const done = waitUntil([&probe] { return probe.finished.load() == 4 * PerKeyCount; });
        INFO("finished " << probe.finished.load() << " of " << 4 * PerKeyCount);
        REQUIRE(done);
    }
    CHECK(probe.overlaps.load() == 0);
    // Four keys on four threads, each holding its strand for 200us at a time: a KeyedStrands that
    // serialised every key through one strand would never have two busy at once.
    CHECK(probe.maxDistinct.load() >= 2);
}

TEST_CASE("Two keys each waiting for the other to start both finish", "[KeyedStrands][threads]")
{
    // A rendezvous only concurrency can satisfy: each task holds its key's strand until the other
    // key's task has started. Serialised through one strand, the first would wait out its budget.
    auto started = std::array<std::atomic<bool>, 2> {};
    auto met = std::array<std::atomic<bool>, 2> {};
    auto finished = std::atomic<int> { 0 };
    auto pool = core::async::ThreadPoolExecutor { 2 };
    {
        auto strands = Strands { pool };
        for (auto const key: { 0, 1 })
            [](Strands* on, int mine, decltype(started)* begun, decltype(met)* both, std::atomic<int>* done)
                -> DetachedTask {
                co_await on->resumeOn(mine);
                auto const me = static_cast<std::size_t>(mine);
                begun->at(me).store(true);
                auto const deadline = std::chrono::steady_clock::now() + 10s;
                while (!begun->at(1 - me).load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                both->at(me).store(begun->at(1 - me).load());
                done->fetch_add(1);
            }(&strands, key, &started, &met, &finished);
        REQUIRE(waitUntil([&finished] { return finished.load() == 2; }));
    }
    CHECK(met[0].load());
    CHECK(met[1].load());
}

TEST_CASE("A key's strand retiring while other threads re-activate that key never runs two of its tasks "
          "at once",
          "[KeyedStrands][threads]")
{
    // The retirement race: the pump finds the queue dry and asks the registry to retire it while a
    // submit on another thread is looking the key up. Either the submit lands first and the strand
    // stays, or the retirement does and the submit makes a new strand -- never both strands alive
    // with work, which would run two tasks of one key at once. Producers pause now and then so the
    // key goes idle and is retired over and over while they run.
    static constexpr auto PerProducer = 20000;
    auto probe = PerKey {};
    auto pool = core::async::ThreadPoolExecutor { 4 };
    {
        auto strands = Strands { pool };
        auto producersDone = std::atomic<int> { 0 };
        auto producer = [&strands, &probe, &producersDone] {
            for (auto const index: std::views::iota(0, PerProducer))
            {
                keyedTick(&strands, &probe, 0);
                if (index % 64 == 0)
                    std::this_thread::yield();
            }
            producersDone.fetch_add(1);
        };
        auto first = std::thread { producer };
        auto second = std::thread { producer };
        // A post never blocks, so a producer that has not finished within the budget is wedged:
        // said here, before the joins below would hang on it.
        CHECK(waitUntil([&producersDone] { return producersDone.load() == 2; }));
        first.join();
        second.join();
        CHECK(waitIdleWithin(strands));
        CHECK(strands.size() == 0);
    }
    CHECK(probe.finished.load() == 2 * PerProducer);
    CHECK(probe.overlaps.load() == 0);
}

TEST_CASE("waitIdle waits for every key's work, including work that work submits", "[KeyedStrands][threads]")
{
    auto finished = std::atomic<int> { 0 };
    auto pool = core::async::ThreadPoolExecutor { 4 };
    {
        auto strands = Strands { pool };
        for (auto const key: { 0, 1, 2, 3, 4, 5, 6, 7 })
            [](Strands* on, int mine, std::atomic<int>* done) -> DetachedTask {
                co_await on->resumeOn(mine);
                std::this_thread::sleep_for(2ms);
                // A follow-up on the next key, submitted from inside the work being waited for.
                [](Strands* again, int next, std::atomic<int>* count) -> DetachedTask {
                    co_await again->resumeOn(next);
                    std::this_thread::sleep_for(2ms);
                    count->fetch_add(1);
                }(on, mine + 1, done);
                done->fetch_add(1);
            }(&strands, key, &finished);

        CHECK(waitIdleWithin(strands));
        // Every one of the sixteen, not merely the eight submitted from here.
        CHECK(finished.load() == 16);
        CHECK(strands.size() == 0);
    }
}

TEST_CASE("waitIdle returns after a key's first hand-off was refused", "[KeyedStrands][exceptions][threads]")
{
    auto base = RefusingBase {};
    auto strands = Strands { base };
    auto order = std::vector<int> {};
    auto task = append(&strands, 9, &order, 1);
    task.handle().resume(); // the refusal reaches the Task, which keeps it for its awaiter
    REQUIRE(task.done());
    CHECK_THROWS_AS(task.result(), std::runtime_error);
    REQUIRE(strands.size() == 0); // or waitIdle below would never return
    CHECK(waitIdleWithin(strands));
    CHECK(order.empty());
}

TEST_CASE("Work offered to many keys while another thread seals them either runs or is handed back",
          "[KeyedStrands][seal][threads]")
{
    static constexpr auto RefusalsEach = 64;
    static constexpr auto OffersEachSide = 1 << 14;
    auto pool = core::async::ThreadPoolExecutor { 4 };
    auto strands = Strands { pool };
    auto ran = std::atomic<int> { 0 };
    auto offered = std::atomic<int> { 0 };
    auto refusedTotal = std::atomic<int> { 0 };
    auto producersDone = std::atomic<int> { 0 };
    auto sealIssued = std::atomic<bool> { false };
    auto const deadline = std::chrono::steady_clock::now() + Budget;

    auto producer = [&](int firstKey) {
        auto refused = 0;
        auto index = 0;
        auto beforeSeal = 0;
        auto afterSeal = 0;
        // Bounded as the Strand case is, on both sides of the seal: a producer that got through its
        // share first waits for the seal, and a seal that refuses nothing fails once the share after
        // it is spent, not on the budget.
        while (refused < RefusalsEach && std::chrono::steady_clock::now() < deadline)
        {
            if (!sealIssued.load())
            {
                if (beforeSeal == OffersEachSide)
                {
                    std::this_thread::yield();
                    continue;
                }
                ++beforeSeal;
            }
            else if (afterSeal++ == OffersEachSide)
                break;
            auto call = [&ran] {
                ran.fetch_add(1);
            };
            // Keys come and go: some have a strand when the seal lands, some would need a new one.
            if (!strands.tryPost(firstKey + (index++ % 16), call))
            {
                ++refused;
                call();
            }
            offered.fetch_add(1);
        }
        refusedTotal.fetch_add(refused);
        producersDone.fetch_add(1);
    };
    auto first = std::thread { producer, 0 };
    auto second = std::thread { producer, 8 };
    while (offered.load() < 2000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    strands.seal();
    sealIssued.store(true);
    CHECK(waitUntil([&producersDone] { return producersDone.load() == 2; }));
    first.join();
    second.join();
    CHECK(waitIdleWithin(strands)); // sealed and drained
    CHECK(strands.size() == 0);
    CHECK(refusedTotal.load() >= 2 * RefusalsEach);
    CHECK(ran.load() == offered.load());
}

#endif
