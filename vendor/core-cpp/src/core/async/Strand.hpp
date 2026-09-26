// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `Strand` — an executor that runs what it is given one at a time, in order, on another.
///
/// A strand removes the need for a lock around state that several coroutines touch: everything
/// that touches it runs on the strand, and the strand never runs two things at once, whatever
/// executor is underneath -- a thread pool included. `KeyedStrands` is the same thing, one per
/// key. The design, and why a coroutine that parks on something another thread completes comes
/// back to the strand, are in `docs/design/strands.md`.

#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>

#include <algorithm>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

// Internal to core::async, not an option: whether this build has threads, and so whether a strand
// can be running a task on a thread other than the one destroying it. Single-threaded Emscripten has
// none: there the wait below cannot be needed, and a blocking wait is not allowed in the WebAssembly
// subset (.agent/rules/library-hygiene.md).
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #include <condition_variable>

    #define CORE_CPP_ASYNC_HAS_THREADS 1
#else
    #define CORE_CPP_ASYNC_HAS_THREADS 0
#endif

namespace core::async
{

namespace detail
{
    class StrandCore;
    class StrandTask;
} // namespace detail

/// What a task a strand runs is, as an around-task hook sees it (@c RunTask::kind).
enum class TaskKind : std::uint8_t
{
    /// A callable handed to `post` or `tryPost`.
    Callable,

    /// A coroutine resumed on the strand: handed to `submit` or `trySubmit`, as a handle or as
    /// `ParkedWork`, which is how a `ResumeOn` hop arrives and how a coroutine that parked off the
    /// strand comes back -- a `KeyedStrands` key's included, through a retired strand's reroute.
    Resumption,
};

/// The rest of one task, handed to an around-task hook: calling it runs the task.
///
/// A hook calls it exactly once, on the thread the hook was called on, before the hook returns.
/// What the task throws propagates through the hook as it would without one.
class RunTask final
{
  public:
    /// Runs the task.
    void operator()() const;

    /// @return Whether the task is a posted callable or a coroutine resumption, so a hook can
    ///         scope what it installs: a request's session to the resumptions of the coroutine
    ///         that serves the request, say, and not to every callable posted to the same strand
    ///         (core-cpp#53). Read from what the task already holds, so it costs one load.
    [[nodiscard]] TaskKind kind() const noexcept;

  private:
    friend class detail::StrandCore;

    /// @param task The task to run.
    explicit RunTask(detail::StrandTask& task) noexcept: _task(&task) {}

    detail::StrandTask* _task;
};

/// A hook a strand calls around every task it runs, instead of running the task itself: to install
/// an ambient context -- a request's session, a tenant -- for exactly the length of each task.
///
/// A task is one resumption, so the hook also runs around a coroutine that parked on another
/// executor and came back through the strand; @c RunTask::kind tells a resumption from a posted
/// callable. Set at construction and never changed; a reference,
/// not an owner: the hook must outlive the strand. Unset, it costs one branch per task and nothing
/// else.
struct AroundTask
{
    /// Called with @c context and the task; must call the task (see @c RunTask).
    void (*call)(void* context, RunTask run) = nullptr;
    /// Passed to @c call.
    void* context = nullptr;

    /// @tparam Hook A callable taking a @c RunTask.
    /// @param hook The hook; referenced, so it must outlive every strand given the result.
    /// @return A hook that calls @p hook.
    template <typename Hook>
        requires std::invocable<Hook&, RunTask>
    [[nodiscard]] static AroundTask of(Hook& hook) noexcept
    {
        return AroundTask {
            .call = [](void* context, RunTask run) { (*static_cast<Hook*>(context))(run); },
            .context = std::addressof(hook),
        };
    }

    /// @return Whether a hook is set.
    [[nodiscard]] explicit operator bool() const noexcept { return call != nullptr; }
};

/// How a strand shares its base executor.
struct StrandOptions
{
    /// How many tasks one turn on the base runs before the strand hands the base back and queues
    /// itself again. At least 1.
    ///
    /// A strand with a long queue would otherwise hold a pool thread, or an event loop's turn, for
    /// as long as work keeps arriving; handing back is what lets an `EventLoop`'s own
    /// `dispatchBatch` bound mean anything with a strand on it. Each hand-back costs one `submit`
    /// on the base.
    std::size_t batch { 32 };

    /// Called around every task the strand runs. `KeyedStrands` takes its hook as a
    /// `KeyedAroundTask`, which is given the key, and asserts that this one is unset.
    AroundTask aroundTask {};
};

namespace detail
{

    /// Where a strand's pump is. The one state the strand's mutex guards besides its queue.
    enum class StrandPhase : std::uint8_t
    {
        Idle = 0,  ///< Suspended and queued nowhere: the next submit queues it on the base.
        Scheduled, ///< Queued on the base, not yet running.
        Running,   ///< Running tasks, on whichever thread the base resumed it on.
        Exited,    ///< Ended, because the strand was closed or retired.
    };

    /// Whether a strand ends when it runs out of work, which is what `KeyedStrands` does with one
    /// key's strand.
    enum class StrandReclaim : std::uint8_t
    {
        Never = 0, ///< It waits, idle, for the next submit.
        WhenIdle,  ///< It asks its owner to retire it, and ends if the owner does.
    };

    /// What a strand asking to be retired may let its owner do with its pump.
    enum class PumpOnRetire : std::uint8_t
    {
        End = 0, ///< The pump ends: the strand will not be reused.
        MayWait, ///< The owner may keep the strand for reuse, its pump waiting, idle, to be queued again.
    };

    /// What a sealed strand does with work offered to it.
    enum class WhenSealed : std::uint8_t
    {
        Admit = 0, ///< A `post` or `submit`: admitted until the strand is closed, since it is how work
                   ///< the strand already admitted comes back.
        Refuse,    ///< A `tryPost` or `trySubmit`: refused, and left with the caller.
    };

    /// The owner's answer to a strand that asked to be retired.
    enum class RetireAnswer : std::uint8_t
    {
        Declined = 0, ///< Not retired: work arrived, or the strand is no longer the key's.
        PumpEnds,     ///< Retired; the pump ends.
        PumpWaits,    ///< Retired and kept for reuse; the pump waits, idle, for the next key's work.
    };

    /// The coroutine that runs a strand's tasks on the strand's base executor.
    ///
    /// One per strand, alive for as long as the strand has one: a strand that goes idle suspends
    /// it rather than ending it, so the steady state costs no allocation. It holds the strand's
    /// state, not the strand, so a strand destroyed while its pump is queued on the base leaves it
    /// something to run: the pump finds the state closed and ends.
    class StrandPump final
    {
      public:
        /// The promise; the standard looks up `StrandPump::promise_type`.
        struct promise_type
        {
            /// @param pumped The strand this pump runs; the same pointer the body takes.
            explicit promise_type(std::shared_ptr<StrandCore> const& pumped) noexcept: strand(pumped) {}

            /// @return The pump, suspended before its first statement.
            [[nodiscard]] StrandPump get_return_object() noexcept
            {
                return StrandPump { std::coroutine_handle<promise_type>::from_promise(*this) };
            }

            /// Suspended until the first submit queues it on the base.
            [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

            /// Frees the frame at the end: a pump ends only when nothing will queue it again.
            [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }

            void return_void() const noexcept {}

            /// A task threw out of `resume()`. Defined after @c StrandCore.
            [[noreturn]] void unhandled_exception();

            /// The strand, for @c unhandled_exception, which cannot reach the body's parameter.
            std::shared_ptr<StrandCore> strand;
        };

        /// @param handle The pump's frame.
        explicit StrandPump(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}

        /// @return The pump's frame.
        [[nodiscard]] std::coroutine_handle<promise_type> handle() const noexcept { return _handle; }

      private:
        std::coroutine_handle<promise_type> _handle;
    };

    /// Runs @p core's tasks, a batch per turn on its base, for as long as the strand lives.
    /// @param strand The strand; held, so a pump queued on the base outlives a closed strand safely.
    /// @return The pump, suspended.
    inline StrandPump runStrandPump(std::shared_ptr<StrandCore> strand);

    /// Keeps the frame of a pump that ended by exception until it is safe to free.
    ///
    /// A coroutine whose `resume()` throws is left suspended at its final point, and nothing may
    /// free it until that `resume()` has returned -- the compiler still marks the frame on the way
    /// out. The thread it threw on is the one thread that knows when that is: the next time it
    /// buries one, or when it exits, the previous one is long finished. So at most one dead pump
    /// per thread is ever held.
    class DeadPumpReaper final
    {
      public:
        DeadPumpReaper() noexcept = default;
        DeadPumpReaper(DeadPumpReaper const&) = delete;
        DeadPumpReaper(DeadPumpReaper&&) = delete;
        DeadPumpReaper& operator=(DeadPumpReaper const&) = delete;
        DeadPumpReaper& operator=(DeadPumpReaper&&) = delete;

        ~DeadPumpReaper()
        {
            if (_dead)
                _dead.destroy();
        }

        /// Frees the pump buried before, and keeps @p dead in its place.
        /// @param dead A pump whose `resume()` is throwing on this thread right now.
        void bury(std::coroutine_handle<> dead) noexcept
        {
            if (auto const previous = std::exchange(_dead, dead))
                previous.destroy();
        }

      private:
        std::coroutine_handle<> _dead;
    };

    /// Hands @p dead to the calling thread's @c DeadPumpReaper.
    /// @param dead A pump whose `resume()` is throwing on this thread right now.
    inline void buryDeadPump(std::coroutine_handle<> dead) noexcept
    {
        thread_local DeadPumpReaper reaper;
        reaper.bury(dead);
    }

    /// Marks the calling thread as freeing work a refused hand-off abandoned, for as long as it
    /// lives: a submit the freed frames' destructors make to the same strand -- or, for a keyed
    /// strand, to any key of the same `KeyedStrands` -- on this thread is dropped, as a closed
    /// strand drops it, instead of scheduling a pump on the base that has just refused one and
    /// throwing out of a destructor.
    ///
    /// Only the destructors themselves: a task that one of them starts on an executor that runs it
    /// inline -- another strand over an inline base -- runs under an executor scope of its own, and
    /// what it submits here is not a resubmit of abandoned work, so it is handed on as usual.
    class FreeingAbandoned final
    {
      public:
        /// @param owner The strand, or the keyed family, whose work is being freed.
        explicit FreeingAbandoned(void const* owner) noexcept:
            _previous(std::exchange(slot(), Freeing { .owner = owner, .scope = ExecutorScope::innermost() }))
        {
        }
        FreeingAbandoned(FreeingAbandoned const&) = delete;
        FreeingAbandoned(FreeingAbandoned&&) = delete;
        FreeingAbandoned& operator=(FreeingAbandoned const&) = delete;
        FreeingAbandoned& operator=(FreeingAbandoned&&) = delete;
        ~FreeingAbandoned() { slot() = _previous; }

        /// @param owner A strand, or a keyed family.
        /// @return Whether the calling thread is freeing work @p owner abandoned, and is not inside
        ///         a task some executor started since.
        [[nodiscard]] static bool active(void const* owner) noexcept
        {
            auto const& freeing = slot();
            return freeing.owner == owner && freeing.scope == ExecutorScope::innermost();
        }

      private:
        /// What is being freed, and the executor scope that was innermost when it began.
        struct Freeing
        {
            void const* owner { nullptr };
            ExecutorScope const* scope { nullptr };
        };

        [[nodiscard]] static Freeing& slot() noexcept
        {
            constinit thread_local Freeing freeing {};
            return freeing;
        }

        Freeing _previous;
    };

    /// A callable posted to a strand, with its type erased: the one allocation a post costs.
    class PostedCall
    {
      public:
        PostedCall() noexcept = default;
        PostedCall(PostedCall const&) = delete;
        PostedCall(PostedCall&&) = delete;
        PostedCall& operator=(PostedCall const&) = delete;
        PostedCall& operator=(PostedCall&&) = delete;

        /// Calls the callable.
        virtual void run() = 0;

        /// Destroys the callable and frees this.
        virtual void destroy() noexcept = 0;

      protected:
        ~PostedCall() = default;
    };

    /// Frees a @c PostedCall.
    struct PostedCallDeleter
    {
        void operator()(PostedCall* call) const noexcept { call->destroy(); }
    };

    /// A posted call, owned.
    using PostedCallPtr = std::unique_ptr<PostedCall, PostedCallDeleter>;

    /// Storage for one @c PostedCallOf, allocated before it is known whether the call will be
    /// queued: a `tryPost` decides that under the strand's lock, and allocates nothing there.
    /// @tparam Call The call type.
    template <typename Call>
    class CallStorage final
    {
      public:
        CallStorage(): _storage(allocate()) {}
        CallStorage(CallStorage const&) = delete;
        CallStorage(CallStorage&&) = delete;
        CallStorage& operator=(CallStorage const&) = delete;
        CallStorage& operator=(CallStorage&&) = delete;

        /// Frees the storage if no call was made in it.
        ~CallStorage()
        {
            if (_storage != nullptr)
                deallocate(_storage);
        }

        /// Makes the call in the storage, which it then owns.
        /// @param fn What the call is made from.
        /// @return The call.
        /// @throws What constructing the callable throws, with the storage kept.
        template <typename Arg>
        [[nodiscard]] PostedCallPtr construct(Arg&& fn)
        {
            auto* const call = ::new (_storage) Call(std::forward<Arg>(fn));
            _storage = nullptr;
            return PostedCallPtr { call };
        }

        /// @return Storage for one call.
        [[nodiscard]] static void* allocate()
        {
            if constexpr (alignof(Call) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                return ::operator new(sizeof(Call), std::align_val_t { alignof(Call) });
            else
                return ::operator new(sizeof(Call));
        }

        /// @param storage What @c allocate returned.
        static void deallocate(void* storage) noexcept
        {
            if constexpr (alignof(Call) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
                ::operator delete(storage, sizeof(Call), std::align_val_t { alignof(Call) });
            else
                ::operator delete(storage, sizeof(Call));
        }

      private:
        void* _storage;
    };

    /// A posted callable of type @p F, held by value.
    /// @tparam F The callable's type.
    template <typename F>
    class PostedCallOf final: public PostedCall
    {
      public:
        /// @param fn The callable, moved or copied in.
        template <typename Arg>
        explicit PostedCallOf(Arg&& fn): _fn(std::forward<Arg>(fn))
        {
        }

        PostedCallOf(PostedCallOf const&) = delete;
        PostedCallOf(PostedCallOf&&) = delete;
        PostedCallOf& operator=(PostedCallOf const&) = delete;
        PostedCallOf& operator=(PostedCallOf&&) = delete;

        void run() override { _fn(); }

        void destroy() noexcept override
        {
            auto* const storage = static_cast<void*>(this);
            this->~PostedCallOf();
            CallStorage<PostedCallOf>::deallocate(storage);
        }

      private:
        ~PostedCallOf() = default;

        F _fn;
    };

    /// One task on a strand's queue: a parked coroutine, or a posted call.
    class StrandTask final
    {
      public:
        StrandTask() noexcept = default;

        /// @param parked A coroutine to resume, and its claim.
        explicit StrandTask(Parked parked) noexcept: _parked(std::move(parked)) {}

        /// @param call A call to make.
        explicit StrandTask(PostedCallPtr call) noexcept: _call(std::move(call)) {}

        /// @return What a submitter names to take this task back out: the call, or the
        ///         coroutine's frame.
        [[nodiscard]] void const* identity() const noexcept
        {
            return _call ? static_cast<void const*>(_call.get()) : _parked.handle().address();
        }

        /// @return Whether this holds nothing: run, given up, or never set.
        [[nodiscard]] bool empty() const noexcept { return !_call && !_parked.handle(); }

        /// @return Whether this is a call or a coroutine. Asked before @c run, which empties it.
        [[nodiscard]] TaskKind kind() const noexcept
        {
            return _call ? TaskKind::Callable : TaskKind::Resumption;
        }

        /// Runs the task and leaves this empty. A call is freed when it returns, or throws.
        void run()
        {
            assert(!empty() && "a strand task ran twice");
            if (auto const call = std::exchange(_call, {}))
                call->run();
            else
                _parked.resume();
        }

        /// Gives the task up without running it, for a submitter that is about to be told no: a
        /// coroutine's claim is disarmed, not released, because its submitter resumes it with the
        /// exception; a call, which only the strand holds, is freed.
        void disarm() noexcept
        {
            if (_parked.handle())
            {
                auto const work = _parked.take();
                work.abandon.disarm();
            }
            _call.reset();
        }

      private:
        Parked _parked;
        PostedCallPtr _call;
    };

    /// A first-in first-out queue of strand tasks that allocates nothing until it is first used.
    ///
    /// A `std::deque` allocates its map and a first block when it is constructed, on some standard
    /// libraries, and `KeyedStrands` constructs a strand every time a key goes from idle to busy.
    class StrandQueue final
    {
      public:
        /// @return Whether nothing is queued.
        [[nodiscard]] bool empty() const noexcept { return _head == _entries.size(); }

        /// @return How many entries are queued.
        [[nodiscard]] std::size_t size() const noexcept { return _entries.size() - _head; }

        /// Makes room for one more entry, so that the next @c push cannot throw.
        void reserveOne()
        {
            if (_entries.size() == _entries.capacity())
                _entries.reserve(std::max(MinimumCapacity, 2 * _entries.capacity()));
        }

        /// Appends @p entry. @pre @c reserveOne since the last push, so this cannot throw.
        /// @param entry The work to queue.
        void push(StrandTask entry) noexcept
        {
            assert(_entries.size() < _entries.capacity());
            _entries.push_back(std::move(entry));
        }

        /// Takes the queued entry @p identity names out of the queue, wherever it is.
        /// @param identity What @c StrandTask::identity answered for it.
        /// @return Its entry, or an empty one where it is not queued.
        [[nodiscard]] StrandTask remove(void const* identity) noexcept
        {
            auto const first = _entries.begin() + static_cast<std::ptrdiff_t>(_head);
            auto const found =
                std::ranges::find_if(first, _entries.end(), [identity](StrandTask const& entry) {
                    return entry.identity() == identity;
                });
            if (identity == nullptr || found == _entries.end())
                return StrandTask {};
            auto entry = std::move(*found);
            _entries.erase(found);
            if (_head == _entries.size())
            {
                _entries.clear();
                _head = 0;
            }
            return entry;
        }

        /// Takes the oldest entry. @pre `!empty()`.
        /// @return The oldest entry.
        [[nodiscard]] StrandTask pop() noexcept
        {
            auto entry = std::move(_entries[_head]);
            ++_head;
            if (_head == _entries.size())
            {
                _entries.clear(); // keeps the capacity
                _head = 0;
            }
            else if (_head >= CompactAfter && _head * 2 >= _entries.size())
            {
                // A strand that never runs dry would otherwise keep every entry it ever took.
                _entries.erase(_entries.begin(), _entries.begin() + static_cast<std::ptrdiff_t>(_head));
                _head = 0;
            }
            return entry;
        }

        /// Takes everything queued, leaving this empty.
        /// @return What was queued, oldest first from @c head.
        [[nodiscard]] std::vector<StrandTask> takeAll() noexcept
        {
            _entries.erase(_entries.begin(), _entries.begin() + static_cast<std::ptrdiff_t>(_head));
            _head = 0;
            return std::exchange(_entries, {});
        }

      private:
        static constexpr std::size_t CompactAfter = 64;
        static constexpr std::size_t MinimumCapacity = 4;

        std::vector<StrandTask> _entries;
        std::size_t _head { 0 };
    };

    /// What a strand is: a queue, a pump, and the state that says where the pump is.
    ///
    /// Shared between the strand's owner, its pump, and every @c ResumeTarget taken inside one of
    /// its tasks, so that neither closing the strand while the pump is queued on the base -- which
    /// the owner cannot take back -- nor a coroutine that parked on the strand and is handed back
    /// after the owner is gone finds freed storage: they find this, closed, and it drops what it is
    /// given. It is the @c IExecutor tasks see as current, for that reason: the owner object -- a
    /// `Strand`, or a `KeyedStrands` -- may be gone by the time a parked coroutine comes back.
    class StrandCore: public IExecutor, public std::enable_shared_from_this<StrandCore>
    {
      public:
        /// @param base Where the pump runs. Must outlive every pump this strand makes.
        /// @param options The batch bound.
        /// @param family The address `ExecutorScope::family()` answers inside a task, or null.
        /// @param reclaim Whether the strand ends when it runs out of work.
        StrandCore(IExecutor& base, StrandOptions options, void const* family, StrandReclaim reclaim) noexcept
            :
            _base(base), _options(options), _family(family), _reclaim(reclaim)
        {
            assert(options.batch > 0
                   && "StrandOptions::batch must be at least 1: a turn that runs nothing never empties "
                      "the strand");
        }

        StrandCore(StrandCore const&) = delete;
        StrandCore(StrandCore&&) = delete;
        StrandCore& operator=(StrandCore const&) = delete;
        StrandCore& operator=(StrandCore&&) = delete;
        ~StrandCore() override = default;

        using IExecutor::submit;

        /// Queues @p handle, borrowed.
        /// @param handle The coroutine to resume on the strand.
        void submit(std::coroutine_handle<> handle) override { submit(ParkedWork { .resume = handle }); }

        /// Queues @p work; a closed strand drops it, and a retired one hands it to its owner.
        ///
        /// **What this throws, it throws with nothing changed**: the work is not queued, and its
        /// claim is disarmed rather than released, because the caller -- `ResumeOn::await_suspend`
        /// -- is about to resume the coroutine with the exception, and releasing the claim of a
        /// detached chain would free the frame that is about to run.
        /// @param work The coroutine to resume on the strand, and its claim on the chain root.
        /// @throws What the base's `submit` throws, and `std::bad_alloc`.
        void submit(ParkedWork work) override
        {
            auto enqueued = Enqueued {};
            auto rerouted = std::optional<ParkedWork> {};
            try
            {
                auto const lock = std::scoped_lock { _mutex };
                // `work` drops when this returns, outside the lock, freeing what nobody owns. Not
                // refused by a seal: this is how a coroutine the strand admitted comes back.
                if (_closed || FreeingAbandoned::active(abandonOwner()))
                    return;
                if (_retired)
                    rerouted.emplace(std::move(work));
                else
                    enqueued = enqueueLocked([&work] { return StrandTask { Parked { std::move(work) } }; });
            }
            catch (...)
            {
                work.abandon.disarm();
                throw;
            }
            // Both outside the lock: a base that resumes inline would run the pump -- and with it
            // the strand's tasks -- inside it, and a reroute takes the owner's lock.
            if (rerouted)
                reroute(std::move(*rerouted));
            else if (enqueued.pump)
                queueOnBase(enqueued.pump, enqueued.identity);
        }

        /// Queues @p work unless the strand is closed. Only for a strand that never retires.
        /// @param work The coroutine to resume on the strand; moved from only where this returns true.
        /// @return Whether it was queued.
        /// @throws What the base's `submit` throws, and `std::bad_alloc`; @p work is then as it was,
        ///         its claim untouched, unless the base refused it, which disarms the claim.
        [[nodiscard]] bool trySubmit(ParkedWork& work)
        {
            auto enqueued = Enqueued {};
            {
                auto const lock = std::scoped_lock { _mutex };
                if (_closed || _sealed || FreeingAbandoned::active(abandonOwner()))
                    return false;
                assert(!_retired && "a keyed strand is offered work through its registry");
                enqueued = enqueueLocked([&work] { return StrandTask { Parked { std::move(work) } }; });
            }
            if (enqueued.pump)
                queueOnBase(enqueued.pump, enqueued.identity);
            return true;
        }

        /// Queues a call made from @p fn unless the strand is closed, or sealed and @p whenSealed
        /// refuses. Only for a strand that never retires. The call is allocated before the lock is
        /// taken and made under it, so a refusal leaves @p fn as it was.
        /// @param fn The callable.
        /// @param whenSealed Whether a seal refuses it.
        /// @return Whether it was queued.
        /// @throws What the base's `submit` throws, what making the call throws, `std::bad_alloc`.
        template <typename Arg>
        [[nodiscard]] bool offerCall(Arg&& fn, WhenSealed whenSealed)
        {
            auto storage = CallStorage<PostedCallOf<std::decay_t<Arg>>> {};
            auto enqueued = Enqueued {};
            {
                auto const lock = std::scoped_lock { _mutex };
                if (_closed || (_sealed && whenSealed == WhenSealed::Refuse)
                    || FreeingAbandoned::active(abandonOwner()))
                    return false;
                assert(!_retired && "a keyed strand is offered work through its registry");
                enqueued = enqueueLocked(
                    [&storage, &fn] { return StrandTask { storage.construct(std::forward<Arg>(fn)) }; });
            }
            if (enqueued.pump)
                queueOnBase(enqueued.pump, enqueued.identity);
            return true;
        }

        /// What @c enqueue queued: the pump to hand to @c queueOnBase once every lock is released,
        /// or an empty handle, and what names the task to take it back out if the base refuses.
        struct Enqueued
        {
            std::coroutine_handle<> pump;
            void const* identity { nullptr };
        };

        /// Queues the task @p make makes, when the caller already knows this strand is open and
        /// not retired: `KeyedStrands` holds its registry's lock across the lookup and this.
        /// @param make Makes the task, called once the steps that can throw for lack of memory are
        ///        done; what it throws leaves nothing queued.
        /// @return What was queued.
        template <typename Make>
        [[nodiscard]] Enqueued enqueue(Make make)
        {
            auto const lock = std::scoped_lock { _mutex };
            assert(!_closed && !_retired);
            return enqueueLocked(std::move(make));
        }

        /// Hands @p pump, which @c enqueue answered, to the base.
        ///
        /// **A refusal abandons the strand's queued work.** A base whose `submit` throws is not
        /// running this strand, so what is queued -- including work other threads queued behind
        /// the scheduled pump and were told was accepted -- is dropped as `close()` drops it:
        /// a chain nobody owns is freed, and a coroutine a `Task` owns is left to its owner. The
        /// caller's own work is taken back out, its claim disarmed, and the exception rethrown to
        /// it. The strand is then idle, and a keyed strand is retired through its owner, so the
        /// next submit starts afresh. A submit that the abandoned frames' destructors make to this
        /// strand as they are freed is dropped (see @c FreeingAbandoned).
        /// @param pump The pump, published as scheduled.
        /// @param withdraw What names the task whose submit published it, to take back on a refusal.
        void queueOnBase(std::coroutine_handle<> pump, void const* withdraw)
        {
            // Held across the base's answer: a base that resumes inline runs the pump inside its
            // `submit`, and a task there may let go of the strand's last owner -- after which the
            // pump ends and frees its own reference too, before the lock below is taken. And on a
            // refusal a keyed strand is erased from its registry.
            auto const keep = shared_from_this();
            try
            {
                _base.submit(pump);
            }
            catch (...)
            {
                auto taken = StrandTask {};
                auto dropped = std::vector<StrandTask> {};
                auto orphan = std::coroutine_handle<> {};
                {
                    auto const lock = std::scoped_lock { _mutex };
                    taken = _queue.remove(withdraw);
                    dropped = _queue.takeAll();
                    orphan = unscheduleLocked();
                    endHandOffLocked();
                }
                taken.disarm();
                freeAbandoned(dropped);
                if (orphan)
                    orphan.destroy();
                retireIdle();
                throw;
            }
            auto const lock = std::scoped_lock { _mutex };
            endHandOffLocked();
        }

        /// @return Whether the calling thread is inside one of this strand's tasks, at any depth.
        [[nodiscard]] bool runningHere() const noexcept
        {
            return ExecutorScope::anyInForce(
                [this](ExecutorScope const& scope) noexcept { return &scope.executor() == this; });
        }

        /// @return How many tasks are queued and not yet running.
        [[nodiscard]] std::size_t queued() const
        {
            auto const lock = std::scoped_lock { _mutex };
            return _queue.size();
        }

        /// @return Whether nothing is queued, and the pump is neither queued on the base nor running.
        [[nodiscard]] bool idle() const
        {
            auto const lock = std::scoped_lock { _mutex };
            return _queue.empty() && _phase != StrandPhase::Scheduled && _phase != StrandPhase::Running;
        }

        /// Closes the strand: queued work is dropped, a task running on another thread is waited
        /// for, and an idle pump is freed. Work that arrives later is dropped. Idempotent.
        void close()
        {
            auto dropped = std::vector<StrandTask> {};
            auto idlePump = std::coroutine_handle<> {};
            {
                auto lock = std::unique_lock { _mutex };
                _closed = true;
#if CORE_CPP_ASYNC_HAS_THREADS
                // Not from inside one of its own tasks, which would wait for itself: the pump ends
                // when that task returns, because it reads `_closed` before it takes another. The
                // hand-offs too: a submit still inside the base's `submit` is about to take its own
                // work back out if the base refuses, and dropping that work here first would free a
                // frame the refusal then resumes.
                if (!runningHere())
                    _settled.wait(lock, [this] { return _phase != StrandPhase::Running && _handOffs == 0; });
#endif
                // Taken after the wait: nothing is queued once `_closed` is set, and a refusal that
                // was in flight has taken its own work back out by now.
                dropped = _queue.takeAll();
                if (_phase == StrandPhase::Idle)
                {
                    idlePump = std::exchange(_pump, {});
                    _phase = StrandPhase::Exited;
                }
            }
            // Outside the lock: freeing a chain runs its destructors, which may submit here again.
            dropped.clear();
            if (idlePump)
                idlePump.destroy();
        }

        /// Closes the offer door: from now on @c trySubmit and a refusing @c offerCall refuse, and
        /// leave the work with the caller. A submit and an admitting post are still admitted -- that
        /// is how work already admitted comes back -- and what is queued keeps running. Idempotent.
        void seal()
        {
            auto const lock = std::scoped_lock { _mutex };
            _sealed = true;
        }

        /// Retires this strand if it has nothing queued: from now on it hands what it is given to
        /// its owner. Called by the owner, holding the owner's own lock.
        /// @return Whether it was retired.
        [[nodiscard]] bool retireIfEmpty() noexcept
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_closed || !_queue.empty())
                return false;
            _retired = true;
            return true;
        }

        /// How many references a strand's own pump holds to it while its frame exists: the
        /// promise's, and the anchor its tasks' scopes hand out. An owner that keeps retired strands
        /// for reuse counts on it: a kept strand whose `use_count()` is its own reference plus these
        /// is referenced by nothing else -- no parked coroutine, no submitter -- and nothing can make
        /// a new reference to it but the owner.
        static constexpr long PumpReferences = 2;

      protected:
        /// Asks the owner to retire this strand, which ran out of work. Only a strand made with
        /// @c StrandReclaim::WhenIdle is asked.
        /// @param pump Whether the owner may keep the strand for reuse, its pump waiting.
        /// @return The owner's answer.
        [[nodiscard]] virtual RetireAnswer tryRetire(PumpOnRetire pump)
        {
            (void) pump;
            return RetireAnswer::Declined;
        }

        /// Puts a retired strand that its owner kept back into service. Called by the owner, holding
        /// its own lock, once nothing else references the strand (see @c PumpReferences).
        void unretire() noexcept
        {
            auto const lock = std::scoped_lock { _mutex };
            assert(_retired && !_closed);
            _retired = false;
        }

        /// Hands @p work, which arrived after this strand retired, to whatever now serves its
        /// work. A strand that never retires is never asked, and drops it if it is.
        /// @param work The work.
        virtual void reroute(ParkedWork work)
        {
            assert(false && "StrandCore::reroute on a strand that never retires");
            auto const dropped = Parked { std::move(work) };
        }

      private:
        friend StrandPump runStrandPump(std::shared_ptr<StrandCore> strand);
        friend struct StrandPump::promise_type;

        /// @return What a @c FreeingAbandoned names for this strand: its keyed family, or itself.
        [[nodiscard]] void const* abandonOwner() const noexcept
        {
            return _family != nullptr ? _family : static_cast<void const*>(this);
        }

        /// Frees what a refused hand-off abandoned, with a resubmit from its destructors dropped.
        /// @param dropped The abandoned work.
        void freeAbandoned(std::vector<StrandTask>& dropped) const noexcept
        {
            auto const freeing = FreeingAbandoned { abandonOwner() };
            dropped.clear();
        }

        /// Ends one hand-off to the base, waking a close that waits for it. Holds the lock.
        void endHandOffLocked() noexcept
        {
            --_handOffs;
            notifySettledLocked();
        }

        /// After a refused hand-off, retires a strand its owner reclaims when idle, and frees the
        /// idle pump it will never queue again.
        void retireIdle()
        {
            if (_reclaim != StrandReclaim::WhenIdle || tryRetire(PumpOnRetire::End) == RetireAnswer::Declined)
                return;
            auto pump = std::coroutine_handle<> {};
            {
                auto const lock = std::scoped_lock { _mutex };
                if (_phase == StrandPhase::Idle)
                {
                    pump = std::exchange(_pump, {});
                    _phase = StrandPhase::Exited;
                }
            }
            if (pump)
                pump.destroy();
        }

        /// Queues the task @p make makes and decides whether the pump must be handed to the base.
        /// Holds the lock.
        ///
        /// Every step that can throw comes before anything is published: the pump's frame is made
        /// first, then room in the queue, then the task -- whose making may throw too, which is a
        /// posted callable's constructor -- and only once it is queued is the pump published as
        /// scheduled.
        /// @param make Makes the task.
        /// @return The pump to hand to the base, or an empty handle, and the task's identity.
        template <typename Make>
        [[nodiscard]] Enqueued enqueueLocked(Make make)
        {
            auto const schedule = _phase == StrandPhase::Idle;
            if (schedule && !_pump)
                _pump = runStrandPump(shared_from_this()).handle();
            _queue.reserveOne();
            auto task = make();
            auto const identity = task.identity();
            _queue.push(std::move(task));
            if (!schedule)
                return {};
            _phase = StrandPhase::Scheduled;
            ++_handOffs; // ended by queueOnBase, whatever the base does
            return Enqueued { .pump = _pump, .identity = identity };
        }

        /// The pump starts a turn. @return False where the strand closed or retired, and the
        /// pump is to end.
        [[nodiscard]] bool beginTurn()
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_closed || _retired)
            {
                _phase = StrandPhase::Exited;
                _pump = {};
                notifySettledLocked();
                return false;
            }
            _phase = StrandPhase::Running;
            return true;
        }

        /// Runs up to one batch of tasks, with this strand current. What a task throws out of
        /// `resume()` propagates, through the pump, to whoever resumed the pump -- except under
        /// MSVC's `cl`, where it ends the process (see `Strand`).
        /// @param anchor What a @c ResumeTarget taken inside a task copies to keep this strand
        ///        alive: the pump's own reference.
        void runBatch(std::shared_ptr<void> const& anchor)
        {
            auto const scope = ExecutorScope { *this, &anchor, _family };
            for ([[maybe_unused]] auto const turn: std::views::iota(std::size_t { 0 }, _options.batch))
            {
                auto entry = StrandTask {};
                {
                    auto const lock = std::scoped_lock { _mutex };
                    if (_closed || _queue.empty())
                        return;
                    entry = _queue.pop();
                }
                runTask(entry);
            }
        }

        /// Runs one task, through the around-task hook if there is one.
        /// @param entry The task.
        void runTask(StrandTask& entry) const
        {
#if defined(_MSC_VER) && !defined(__clang__)
            // A compiler workaround, not platform logic: under `cl` an exception thrown out of a
            // coroutine's `resume()` and on through this frame and the pump's was measured
            // corrupting the thread's executor scope chain (cl-release; cl-debug and clang-cl are
            // fine), after which nothing about the thread can be trusted. So it ends the process
            // here, at the first frame that sees it, with a message saying why -- a posted call's
            // throw and a hook's included, since they take the same way out.
            try
            {
                runTaskAround(entry);
            }
            catch (...)
            {
                taskThrewUnderMsvc();
            }
#else
            runTaskAround(entry);
#endif
        }

        /// @copydoc runTask
        void runTaskAround(StrandTask& entry) const
        {
            auto const& around = _options.aroundTask;
            if (!around)
            {
                entry.run();
                return;
            }
            around.call(around.context, RunTask { entry });
            assert(entry.empty() && "an around-task hook must run the task it is given");
        }

#if defined(_MSC_VER) && !defined(__clang__)
        /// Ends the process for a task that threw out of `resume()`, under MSVC's `cl`.
        [[noreturn]] static void taskThrewUnderMsvc() noexcept
        {
            std::fputs("core::async::Strand: a task threw out of resume() (or a posted call threw); under "
                       "MSVC's cl that exception cannot cross the strand's coroutine frames safely, so "
                       "the process ends (see Strand's documentation)\n",
                       stderr);
            std::fflush(stderr);
            std::terminate();
        }
#endif

        /// The pump's suspension between turns: queued again on the base if work is waiting, idle
        /// if not, and ended if the strand closed or retired.
        class EndTurn final
        {
          public:
            /// @param strand The strand, held by the pump's frame.
            explicit EndTurn(StrandCore& strand) noexcept: _strand(&strand) {}

            /// @return False: whether to suspend is `await_suspend`'s question, asked under the lock.
            [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

            /// Decides where the pump goes next. After it has published "idle" or queued the pump,
            /// another thread may resume it, so nothing reachable through the frame is touched
            /// after either.
            /// @param pump The suspended pump.
            /// @return True to stay suspended; false to go on, into another turn or the end.
            [[nodiscard]] bool await_suspend(std::coroutine_handle<> pump) const
            {
                auto& self = *_strand;
                auto lock = std::unique_lock { self._mutex };
                if (self._closed)
                    return false;
                if (self._queue.empty() && self._reclaim == StrandReclaim::WhenIdle)
                {
                    // Stays Running while the owner decides, so no submit queues the pump in the
                    // meantime: work that arrives now is queued, and seen below.
                    lock.unlock();
                    auto const answer = self.tryRetire(PumpOnRetire::MayWait);
                    if (answer == RetireAnswer::PumpEnds)
                        return false;
                    lock.lock();
                    if (self._closed)
                        return false;
                    // Kept for reuse: the pump waits, idle, until the strand is given to a key and
                    // queued again -- unless it already has been, while the lock was released, in
                    // which case it goes on as any strand does below.
                    if (answer == RetireAnswer::PumpWaits && self._retired)
                    {
                        self._phase = StrandPhase::Idle;
                        self.notifySettledLocked();
                        return true;
                    }
                }
                if (self._queue.empty())
                {
                    self._phase = StrandPhase::Idle;
                    self.notifySettledLocked();
                    return true;
                }
                self._phase = StrandPhase::Scheduled;
                auto& base = self._base;
                lock.unlock();
                try
                {
                    base.submit(pump);
                }
                catch (...)
                {
                    // The base refused to take the pump back. Nobody else can have moved it on --
                    // only the base resumes a scheduled pump -- and there is no caller to tell, so
                    // the pump keeps the thread it is on and runs the next turn now.
                    auto const relock = std::scoped_lock { self._mutex };
                    self._phase = StrandPhase::Running;
                    return false;
                }
                return true;
            }

            void await_resume() const noexcept {}

          private:
            StrandCore* _strand;
        };

        /// A task threw out of `resume()`, which ended the pump: a new one takes over the queue.
        /// Called from the dead pump's `unhandled_exception`, on the thread it threw on.
        ///
        /// Throws with the strand in a state that restarts: if the new pump cannot be made, or the
        /// base refuses it, the strand is left idle with no pump, and the next submit makes one.
        void replaceDeadPump()
        {
            auto fresh = std::coroutine_handle<> {};
            try
            {
                fresh = runStrandPump(shared_from_this()).handle();
            }
            catch (...)
            {
                {
                    auto const lock = std::scoped_lock { _mutex };
                    _pump = {};
                    _phase = _closed ? StrandPhase::Exited : StrandPhase::Idle;
                    notifySettledLocked();
                }
                // A keyed strand left with nothing queued would otherwise stay registered, idle,
                // with no pump to retire it.
                retireIdle();
                throw;
            }

            auto pump = std::coroutine_handle<> {};
            auto discard = std::coroutine_handle<> {};
            {
                auto const lock = std::scoped_lock { _mutex };
                if (_closed)
                {
                    _phase = StrandPhase::Exited;
                    _pump = {};
                    discard = fresh;
                }
                else
                {
                    _pump = fresh;
                    // A strand that is reclaimed when idle is queued even with nothing to do, so
                    // the new pump's first turn is what retires it.
                    if (_queue.empty() && _reclaim == StrandReclaim::Never)
                        _phase = StrandPhase::Idle;
                    else
                    {
                        _phase = StrandPhase::Scheduled;
                        pump = fresh;
                    }
                }
                notifySettledLocked();
            }
            if (discard)
                discard.destroy();
            if (!pump)
                return;
            try
            {
                _base.submit(pump);
            }
            catch (...)
            {
                // A refusal abandons the queue, as it does in queueOnBase; nothing of the caller's
                // is in it here.
                auto dropped = std::vector<StrandTask> {};
                auto orphan = std::coroutine_handle<> {};
                {
                    auto const lock = std::scoped_lock { _mutex };
                    dropped = _queue.takeAll();
                    orphan = unscheduleLocked();
                }
                freeAbandoned(dropped);
                if (orphan)
                    orphan.destroy();
                retireIdle();
                throw;
            }
        }

        /// Takes back a scheduled pump the base refused. Nothing else can have moved it on --
        /// only the base resumes a scheduled pump -- so it is idle again; or, where the strand
        /// closed meanwhile and so will never queue it again, it is handed back to be freed.
        /// Holds the lock.
        /// @return The pump to destroy once the lock is released, or an empty handle.
        [[nodiscard]] std::coroutine_handle<> unscheduleLocked() noexcept
        {
            auto orphan = std::coroutine_handle<> {};
            if (_phase == StrandPhase::Scheduled)
            {
                if (_closed)
                {
                    _phase = StrandPhase::Exited;
                    orphan = std::exchange(_pump, {});
                }
                else
                    _phase = StrandPhase::Idle;
            }
            notifySettledLocked();
            return orphan;
        }

        /// Wakes a close waiting for the pump to stop running. Holds the lock.
        void notifySettledLocked() noexcept
        {
#if CORE_CPP_ASYNC_HAS_THREADS
            _settled.notify_all();
#endif
        }

        IExecutor& _base;
        StrandOptions _options;
        void const* _family;

        /// Guards everything below.
        mutable std::mutex _mutex;
#if CORE_CPP_ASYNC_HAS_THREADS
        std::condition_variable _settled; ///< Signalled whenever the pump stops running.
#endif
        StrandQueue _queue;
        std::coroutine_handle<> _pump;
        StrandPhase _phase { StrandPhase::Idle };
        /// Submits between publishing the pump as scheduled and the base's answer; `close()` waits
        /// for them.
        std::size_t _handOffs { 0 };
        StrandReclaim _reclaim;
        bool _closed { false };  ///< The owner is gone; nothing runs any more.
        bool _sealed { false };  ///< The `try` members refuse; everything else is as before.
        bool _retired { false }; ///< The owner reclaimed it; work goes to the owner.
    };

    inline StrandPump runStrandPump(std::shared_ptr<StrandCore> strand)
    {
        // Moved out of the parameter first, so a pump that dies by exception -- whose frame, and
        // with it the parameter, lives on until the thread's reaper frees it -- pins nothing.
        // Held for the pump's life and handed to every scope a task runs in, so a ResumeTarget taken
        // inside a task keeps the strand's state alive after its owner is gone. With the promise's,
        // it is one of the pump's two references (StrandCore::PumpReferences).
        auto const anchor = std::shared_ptr<void> { std::move(strand) };
        auto& self = *static_cast<StrandCore*>(anchor.get());
        while (self.beginTurn())
        {
            self.runBatch(anchor);
            co_await StrandCore::EndTurn { self };
        }
    }

    inline void StrandPump::promise_type::unhandled_exception()
    {
        auto const self = std::coroutine_handle<promise_type>::from_promise(*this);
        auto const owner = std::move(strand);
        try
        {
            owner->replaceDeadPump();
        }
        catch (...)
        {
            // The replacement could not be made or queued, or its first turn ran inline and threw
            // too: that exception is the one that leaves, and this frame is still buried rather
            // than leaked.
            buryDeadPump(self);
            throw;
        }
        // AFTER queuing the replacement: an inline base runs it inside the call above, and a
        // replacement that died too would bury its own frame, freeing this one while it still
        // unwinds. Buried now, it is freed only once this thread next buries one or exits.
        buryDeadPump(self);
        throw;
    }

} // namespace detail

inline void RunTask::operator()() const
{
    _task->run();
}

inline TaskKind RunTask::kind() const noexcept
{
    return _task->kind();
}

/// An executor that runs what it is given one at a time, in the order given, on a base executor.
///
/// **Serial.** At most one task runs at a time, whatever the base is, so state touched only from
/// the strand needs no lock. Two strands over one pool run concurrently with each other.
///
/// **FIFO.** Tasks run in the order `submit` and `post` received them; `co_await ResumeOn { strand }`
/// is a submit.
///
/// **Callables.** `post(fn)` runs `fn` as one task, held by value in one allocation. `tryPost` and
/// `trySubmit` refuse once the strand is closed and leave the work with the caller.
///
/// **Ambient context.** `StrandOptions::aroundTask` is called around every task -- every
/// resumption, including one that came back through the strand from another executor -- to
/// install per-task context such as a session.
///
/// **What a task is.** A resumption: from `submit` until the coroutine next suspends. A coroutine
/// that suspends has left the strand, and another task may run before it comes back; it comes
/// back to the strand when what it awaited is a `ResumeOn { strand }`, or an awaitable that
/// resumes on the current executor (`AsyncQueue::pop`, see `ExecutorContext.hpp`). Socket and
/// timer awaitables of `core::net` resume on their `EventLoop` instead, and a coroutine hops back
/// with `co_await ResumeOn { strand }`.
///
/// **Current executor.** Inside a task, `runningHere()` is true -- also inside anything the task
/// resumes synchronously -- and `currentExecutor()` is the strand's shared state: an executor that
/// submits to this strand, and not this object's address, so that a coroutine which parks on the
/// strand and is handed back after the strand is destroyed finds that state, closed, rather than
/// freed storage. Ask `runningHere()`, never compare the pointer.
///
/// **A task that throws out of `resume()`** -- which no coroutine type of this module does, since
/// `Task` hands an exception to its awaiter and `DetachedTask` terminates -- propagates to
/// whoever resumed the strand on the base, and the strand goes on with the tasks behind it.
/// **Under MSVC's `cl` it ends the process instead**, with a message on `stderr`: an exception
/// crossing the strand's coroutine frames was measured corrupting the thread's executor scopes
/// there (`core-cpp.strand-throw-canary` watches it).
///
/// **A refused hand-off abandons the strand's queued work.** A base whose `submit` throws when the
/// strand hands it its pump is not running this strand: the `submit` that handed it over throws,
/// with its own work taken back out, and everything else queued -- including work another thread
/// queued behind the scheduled pump and was told was accepted -- is dropped as `close()` drops it. The
/// strand is then idle, and the next submit starts it afresh. A frame freed by that drop whose
/// destructor submits to the strand again -- to any key of the same `KeyedStrands`, for a keyed
/// strand -- has that work dropped too, on that thread, rather than handed to the base that has just
/// refused: it would throw out of a destructor. A refusal between two turns, which nobody could be
/// told about, runs the next turn on the thread the strand already has.
///
/// **Destruction.** Tasks still queued are dropped, never run: a chain rooted in a `DetachedTask`
/// is freed, and a coroutine a `Task` owns is left to its owner, suspended. A task running on
/// another thread is waited for (not where threads do not exist, and not from inside one of the
/// strand's own tasks, which the strand finishes once it returns). So a task may release the last
/// reference to the strand's owner: the destructor returns at once, the task runs to its end, and
/// the pump then ends without running anything more. A coroutine that parked on the strand and
/// is handed back after it is gone -- by an `AsyncQueue` push, close or stop -- is dropped, which
/// frees a chain nobody owns. The base must outlive the strand and run what the strand queued on
/// it: a pump queued there finds the strand closed and ends. **`core::net::EventLoop` as a base
/// drops what is still in its inbound queue when it is destroyed**, so a strand whose pump was
/// handed to a loop from another thread and not yet taken up by a turn leaks its state with the
/// loop; run one more turn, or destroy the strand first. On the single-threaded WebAssembly build
/// nothing else can be running, so destruction drops what is queued without waiting; a host that
/// wants it run pumps its base until `idle()` first.
class Strand final: public IExecutor
{
  public:
    /// @param base Where the strand's tasks run. Must outlive the strand.
    /// @param options How the strand shares its base.
    explicit Strand(IExecutor& base, StrandOptions options = {}):
        _core(std::make_shared<detail::StrandCore>(base, options, nullptr, detail::StrandReclaim::Never))
    {
    }

    Strand(Strand const&) = delete;
    Strand(Strand&&) = delete;
    Strand& operator=(Strand const&) = delete;
    Strand& operator=(Strand&&) = delete;

    /// Drops what is queued and waits for a task running on another thread. See the class.
    ~Strand() override { _core->close(); }

    using IExecutor::submit;

    /// Queues @p handle, borrowed. Callable from any thread.
    /// @param handle The coroutine to resume on the strand.
    /// @throws What the base's `submit` throws, with nothing queued; `std::bad_alloc`.
    void submit(std::coroutine_handle<> handle) override { _core->submit(handle); }

    /// Queues @p work, holding its claim until it runs. Callable from any thread.
    /// @param work The coroutine to resume on the strand, and its claim on the chain root.
    void submit(ParkedWork work) override { _core->submit(std::move(work)); }

    /// @return Whether the calling thread is inside one of this strand's tasks.
    [[nodiscard]] bool runningHere() const noexcept { return _core->runningHere(); }

    /// @return How many tasks are queued and not yet running. Racy by nature; for tests.
    [[nodiscard]] std::size_t queued() const { return _core->queued(); }

    /// Queues @p fn, a callable, to run as one task. Callable from any thread.
    ///
    /// The callable is held by value in one allocation, freed once it has run. What it throws takes
    /// the way a task's throw takes (see the class). A closed strand drops it without calling it; a
    /// sealed one still admits it.
    /// @param fn The callable, called with no arguments; its result is ignored.
    /// @throws What the base's `submit` throws, what copying or moving @p fn throws, `std::bad_alloc`;
    ///         nothing is queued then.
    template <typename F>
        requires std::invocable<std::decay_t<F>&> && std::constructible_from<std::decay_t<F>, F>
    void post(F&& fn)
    {
        std::ignore = _core->offerCall(std::forward<F>(fn), detail::WhenSealed::Admit);
    }

    /// Queues @p fn, unless the strand is closed or sealed -- for work that must run somewhere, which
    /// the caller then runs itself.
    ///
    /// @p fn is moved into the strand under its lock, which is how a closed strand can leave it
    /// untouched: its move constructor must not submit to this strand.
    /// @param fn The callable; moved from only where this returns true.
    /// @return Whether it was queued.
    /// @throws As @c post does.
    template <typename F>
        requires std::invocable<F&> && std::move_constructible<F>
    [[nodiscard]] bool tryPost(F& fn)
    {
        return _core->offerCall(std::move(fn), detail::WhenSealed::Refuse);
    }

    /// Queues @p handle, borrowed, unless the strand is closed or sealed.
    /// @param handle The coroutine to resume on the strand.
    /// @return Whether it was queued.
    /// @throws As @c submit does.
    [[nodiscard]] bool trySubmit(std::coroutine_handle<> handle)
    {
        auto work = ParkedWork { .resume = handle };
        return _core->trySubmit(work);
    }

    /// Queues @p work, unless the strand is closed or sealed.
    /// @param work The coroutine and its claim; moved from only where this returns true.
    /// @return Whether it was queued.
    /// @throws As @c submit does, with @p work as it was.
    [[nodiscard]] bool trySubmit(ParkedWork& work) { return _core->trySubmit(work); }

    /// Closes the strand, as the destructor does: queued work is dropped, a task running on another
    /// thread is waited for, and whatever arrives later is dropped, or refused by the `try`
    /// members. Idempotent; the destructor calls it.
    void close() { _core->close(); }

    /// Closes the offer door, and keeps running what is queued and what comes back: the first step of
    /// a teardown that loses nothing -- `seal()`, then drain, then `close()`.
    ///
    /// After it, `tryPost` and `trySubmit` return false and leave the work with the caller, which
    /// can run it itself. `post` and `submit` are still admitted until `close()`: `submit` is how a
    /// coroutine the strand already admitted comes back -- `ResumeOn`, and an `AsyncQueue` push,
    /// close or stop through its `ResumeTarget` -- and dropping it would free a detached chain
    /// without its finish, or leave its awaiter waiting for ever. Queued work runs as usual.
    ///
    /// **`idle()` then means nothing queued and nothing running, and no more than that.** A
    /// coroutine suspended off the strand -- on a socket, a timer, an `AsyncQueue` -- is invisible
    /// to it and may still come back. A consumer counts its own in-flight work (morph: its stop
    /// signal plus the runs it tracks) and drains until that count and `idle()` both say done, and
    /// only then closes. `post` is new work too: a producer that posts must stop, or offer through
    /// `tryPost`, before the drain. `close()` afterwards behaves as ever. Idempotent; there is no
    /// unsealing.
    void seal() { _core->seal(); }

    /// @return Whether nothing is queued or running -- what a single-threaded host pumps its base
    ///         until, before it destroys the strand. Racy by nature where other threads submit.
    [[nodiscard]] bool idle() const { return _core->idle(); }

  private:
    std::shared_ptr<detail::StrandCore> _core;
};

} // namespace core::async
