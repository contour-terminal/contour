// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `KeyedStrands` — one @c Strand per key, made when a key gets work and reclaimed when it runs
/// out.

#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Strand.hpp>

#include <algorithm>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if CORE_CPP_ASYNC_HAS_THREADS
    #include <condition_variable>
#endif

namespace core::async
{

/// A hook `KeyedStrands` calls around every task any key's strand runs, with the key: to install
/// the ambient context that belongs to that key -- the session of the action the key is running --
/// for exactly the length of each task. What `AroundTask` says holds for it.
/// @tparam Key The key type.
template <typename Key>
struct KeyedAroundTask
{
    /// Called with @c context, the key and the task; must call the task (see @c RunTask).
    void (*call)(void* context, Key const& key, RunTask run) = nullptr;
    /// Passed to @c call.
    void* context = nullptr;

    /// @tparam Hook A callable taking the key and a @c RunTask.
    /// @param hook The hook; referenced, so it must outlive the `KeyedStrands` given the result.
    /// @return A hook that calls @p hook.
    template <typename Hook>
        requires std::invocable<Hook&, Key const&, RunTask>
    [[nodiscard]] static KeyedAroundTask of(Hook& hook) noexcept
    {
        return KeyedAroundTask {
            .call =
                [](void* context, Key const& key, RunTask run) { (*static_cast<Hook*>(context))(key, run); },
            .context = std::addressof(hook),
        };
    }

    /// @return Whether a hook is set.
    [[nodiscard]] explicit operator bool() const noexcept { return call != nullptr; }
};

namespace detail
{

    template <typename Key, typename Hash, typename KeyEqual>
    class KeyedStrandsRegistry;

    /// One key's strand. It is the executor its tasks see as current, and what a @c ResumeTarget
    /// taken in one of them keeps alive, so a coroutine that parked while its key went idle can
    /// still find the key: a retired strand hands what it is given to the registry, which gives it
    /// to the key's current strand, or makes one.
    template <typename Key, typename Hash, typename KeyEqual>
    class KeyStrand final: public StrandCore
    {
      public:
        using Registry = KeyedStrandsRegistry<Key, Hash, KeyEqual>;

        /// @param registry The strands this one belongs to.
        /// @param key The key it serves.
        /// @param base Where its pump runs.
        /// @param options How it shares the base.
        KeyStrand(std::shared_ptr<Registry> registry, Key key, IExecutor& base, StrandOptions options):
            StrandCore(base,
                       withKeyedHook(options, registry->hooked(), this),
                       registry.get(),
                       StrandReclaim::WhenIdle),
            _registry(std::move(registry)),
            _key(std::move(key))
        {
        }

        /// @return The key this strand serves.
        [[nodiscard]] Key const& key() const noexcept { return _key; }

        /// Gives a kept, retired strand to @p key. Called by the registry under its lock, once
        /// nothing else references this strand.
        /// @param key The key it serves from now on.
        void rekey(Key const& key)
        {
            _key = key;
            unretire();
        }

      protected:
        [[nodiscard]] RetireAnswer tryRetire(PumpOnRetire pump) override
        {
            return _registry->retire(*this, pump);
        }

        void reroute(ParkedWork work) override { _registry->submit(_key, std::move(work)); }

      private:
        /// @return @p options, with an around-task hook that hands this strand's key to the
        ///         registry's keyed hook where the registry has one.
        [[nodiscard]] static StrandOptions withKeyedHook(StrandOptions options,
                                                         bool hooked,
                                                         KeyStrand* self) noexcept
        {
            if (hooked)
                options.aroundTask = AroundTask {
                    .call =
                        [](void* context, RunTask run) {
                            auto const& strand = *static_cast<KeyStrand const*>(context);
                            strand._registry->aroundTask(strand._key, run);
                        },
                    .context = self,
                };
            return options;
        }

        std::shared_ptr<Registry> _registry;
        Key _key;
    };

    /// The keys and their strands, shared by the @c KeyedStrands that owns them and by each key's
    /// strand, so a strand a parked coroutine still holds can reach it after the owner is gone.
    template <typename Key, typename Hash, typename KeyEqual>
    class KeyedStrandsRegistry final:
        public std::enable_shared_from_this<KeyedStrandsRegistry<Key, Hash, KeyEqual>>
    {
      public:
        using KeyStrandType = KeyStrand<Key, Hash, KeyEqual>;

        /// @param base Where every key's pump runs.
        /// @param options How each key's strand shares the base.
        /// @param aroundTask Called around every task, with its key; may be unset.
        /// @throws std::bad_alloc Not noexcept: MSVC's `unordered_map` allocates even empty, and the
        ///         room for kept strands is reserved here.
        KeyedStrandsRegistry(IExecutor& base, StrandOptions options, KeyedAroundTask<Key> aroundTask):
            _base(base), _options(options), _aroundTask(aroundTask)
        {
            _spareStrands.reserve(SpareStrands);
            _spareNodes.reserve(SpareStrands);
        }

        /// Queues @p work on @p key's strand, making the strand if the key has none -- also once the
        /// registry is sealed, since this is how a coroutine it admitted comes back. After it closed,
        /// the work is dropped, which frees what nobody owns.
        /// @param key The key.
        /// @param work The coroutine to resume on it.
        void submit(Key const& key, ParkedWork work)
        {
            try
            {
                // `work` drops when this returns, if it was refused, freeing what nobody owns.
                std::ignore = offer(
                    key, WhenSealed::Admit, [&work] { return StrandTask { Parked { std::move(work) } }; });
            }
            catch (...)
            {
                // The caller resumes the coroutine with this, so its claim is given back rather than
                // released.
                work.abandon.disarm();
                throw;
            }
        }

        /// Queues @p work on @p key's strand unless the registry is closed or sealed.
        /// @return Whether it was queued; where not, @p work is as it was.
        [[nodiscard]] bool trySubmit(Key const& key, ParkedWork& work)
        {
            return offer(
                key, WhenSealed::Refuse, [&work] { return StrandTask { Parked { std::move(work) } }; });
        }

        /// Queues a call made from @p fn on @p key's strand unless the registry is closed, or sealed
        /// and @p whenSealed refuses. The call is allocated before any lock is taken and made under it.
        /// @return Whether it was queued; where not, @p fn is as it was.
        template <typename Arg>
        [[nodiscard]] bool offerCall(Key const& key, Arg&& fn, WhenSealed whenSealed)
        {
            auto storage = CallStorage<PostedCallOf<std::decay_t<Arg>>> {};
            return offer(key, whenSealed, [&storage, &fn] {
                return StrandTask { storage.construct(std::forward<Arg>(fn)) };
            });
        }

        /// Calls the keyed around-task hook. @pre @c hooked.
        /// @param key The key whose task @p run is.
        /// @param run The task.
        void aroundTask(Key const& key, RunTask run) const
        {
            _aroundTask.call(_aroundTask.context, key, run);
        }

        /// @return Whether a keyed around-task hook is set.
        [[nodiscard]] bool hooked() const noexcept { return static_cast<bool>(_aroundTask); }

        /// Retires @p strand if it is still @p strand's key's strand and has nothing queued, and
        /// keeps it, and its map node, for the next key that needs a strand while there is room.
        /// @param strand A strand whose pump ran out of work.
        /// @param pump Whether the strand may be kept with its pump waiting.
        /// @return The answer: declined, or retired with its pump ending or waiting.
        [[nodiscard]] RetireAnswer retire(KeyStrandType& strand, PumpOnRetire pump)
        {
            auto const lock = std::scoped_lock { _mutex };
            auto const slot = _strands.find(strand.key());
            if (slot == _strands.end() || slot->second.get() != &strand || !strand.retireIfEmpty())
                return RetireAnswer::Declined;
            auto node = _strands.extract(slot);
            auto answer = RetireAnswer::PumpEnds;
            // Both vectors had their room reserved at construction: keeping allocates nothing.
            if (pump == PumpOnRetire::MayWait && !_closed && !_sealed && _spareStrands.size() < SpareStrands)
            {
                _spareStrands.push_back(std::move(node.mapped()));
                answer = RetireAnswer::PumpWaits;
            }
            if (_spareNodes.size() < SpareStrands)
            {
                node.mapped().reset();
                _spareNodes.push_back(std::move(node));
            }
            notifyIfIdleLocked();
            // A strand not kept is still referenced by its pump, which is running this: dropping
            // the registry's reference here frees nothing.
            return answer;
        }

        /// Closes the offer door for every key. See `KeyedStrands::seal`.
        ///
        /// The registry is the door: every offer to a key -- `tryPost`, `trySubmit` -- comes through
        /// `offer`, which reads this under the same lock it queues under. A key's strand itself is
        /// only ever submitted to, which a seal admits, so it needs no flag of its own.
        void seal()
        {
            auto const lock = std::scoped_lock { _mutex };
            _sealed = true;
        }

        /// Closes every strand and refuses what arrives later. See `~KeyedStrands`.
        void close()
        {
            auto strands = std::unordered_map<Key, std::shared_ptr<KeyStrandType>, Hash, KeyEqual> {};
            auto spares = std::vector<std::shared_ptr<KeyStrandType>> {};
            auto spareNodes = std::vector<NodeType> {};
            {
                auto const lock = std::scoped_lock { _mutex };
                _closed = true;
                strands.swap(_strands);
                spares.swap(_spareStrands);
                spareNodes.swap(_spareNodes);
#if CORE_CPP_ASYNC_HAS_THREADS
                _idle.notify_all();
#endif
            }
            for (auto& [key, strand]: strands)
                strand->close();
            // A kept strand's pump waits, idle, holding the strand: closing it frees the pump, which
            // is what lets the strand -- and the reference it holds to this registry -- go.
            for (auto const& spare: spares)
                spare->close();
        }

        /// @return How many keys have a strand right now.
        [[nodiscard]] std::size_t size() const
        {
            auto const lock = std::scoped_lock { _mutex };
            return _strands.size();
        }

        /// @return Whether no key has a strand: nothing queued or running on any.
        [[nodiscard]] bool idle() const
        {
            auto const lock = std::scoped_lock { _mutex };
            return _strands.empty();
        }

#if CORE_CPP_ASYNC_HAS_THREADS
        /// Blocks until no key has a strand.
        void waitIdle()
        {
            auto lock = std::unique_lock { _mutex };
            _idle.wait(lock, [this] { return _strands.empty() || _closed; });
        }
#endif

      private:
        /// Queues the task @p make makes on @p key's strand, making the strand if the key has none.
        ///
        /// Touches @p key only until the task is queued: it may be a member of the awaiter of the
        /// very coroutine being queued, which another thread can resume, and so destroy, the moment
        /// the strand's lock is released.
        /// @param key The key.
        /// @param whenSealed Whether a seal refuses the task.
        /// @param make Makes the task; not called where this returns false.
        /// @return False where the registry is closed, or sealed and @p whenSealed refuses, or this
        ///         thread is freeing work one of its strands abandoned (see
        ///         `detail::FreeingAbandoned`).
        /// @throws What the base's `submit` throws, what @p make throws, `std::bad_alloc`. A strand
        ///         made for the key that could not take the task is removed again.
        template <typename Make>
        [[nodiscard]] bool offer(Key const& key, WhenSealed whenSealed, Make make)
        {
            if (FreeingAbandoned::active(this))
                return false;
            auto enqueued = StrandCore::Enqueued {};
            // Held, not borrowed: once the pump is queued the strand can run, retire and let go of
            // itself before the hand-off below has been told how the base answered.
            auto strand = std::shared_ptr<KeyStrandType> {};
            // A strand made here that could not take the task: removed under the lock, closed
            // outside it, which frees the pump it may already have made.
            auto discarded = std::shared_ptr<KeyStrandType> {};
            try
            {
                auto const lock = std::scoped_lock { _mutex };
                if (_closed || (_sealed && whenSealed == WhenSealed::Refuse))
                    return false;
                auto slot = _strands.find(key);
                auto const made = slot == _strands.end();
                if (made)
                {
                    auto fresh = strandForLocked(key);
                    try
                    {
                        slot = insertLocked(key, fresh);
                    }
                    catch (...)
                    {
                        // A kept strand's pump waits, holding it: closed below, not just dropped.
                        discarded = std::move(fresh);
                        throw;
                    }
                }
                // Under the registry's lock, so a retirement cannot slip between the lookup and the
                // queueing: `retire` takes this lock first, then the strand's.
                strand = slot->second;
                try
                {
                    enqueued = strand->enqueue(std::move(make));
                }
                catch (...)
                {
                    if (made)
                    {
                        discarded = strand;
                        _strands.erase(slot);
                        notifyIfIdleLocked();
                    }
                    throw;
                }
            }
            catch (...)
            {
                if (discarded)
                    discarded->close();
                throw;
            }
            // Outside every lock: a base that resumes inline runs the strand's tasks in this call.
            if (enqueued.pump)
                strand->queueOnBase(enqueued.pump, enqueued.identity);
            return true;
        }

        /// A strand for @p key, which has none: a kept one that nothing else references, given to
        /// @p key, or a new one. Holds the lock.
        /// @param key The key.
        /// @return The strand.
        [[nodiscard]] std::shared_ptr<KeyStrandType> strandForLocked(Key const& key)
        {
            auto const unreferenced = [](std::shared_ptr<KeyStrandType> const& spare) noexcept {
                return spare.use_count() == 1 + StrandCore::PumpReferences;
            };
            // Sealed, nothing kept is handed out again (nor, in `retire`, kept): a strand made now
            // serves the work that is still coming back, and goes when it runs dry.
            if (auto const found =
                    _sealed ? _spareStrands.end() : std::ranges::find_if(_spareStrands, unreferenced);
                found != _spareStrands.end())
            {
                // Given the key while still kept: if copying the key throws, the strand stays kept.
                (*found)->rekey(key);
                auto strand = std::move(*found);
                *found = std::move(_spareStrands.back());
                _spareStrands.pop_back();
                return strand;
            }
            return std::make_shared<KeyStrandType>(this->shared_from_this(), key, _base, _options);
        }

        /// Maps @p key to @p strand, in a kept map node where there is one. Holds the lock.
        /// @return Where it is mapped.
        [[nodiscard]] auto insertLocked(Key const& key, std::shared_ptr<KeyStrandType> strand)
        {
            if (_spareNodes.empty())
                return _strands.emplace(key, std::move(strand)).first;
            auto node = std::move(_spareNodes.back());
            _spareNodes.pop_back();
            node.key() = key;
            node.mapped() = std::move(strand);
            return _strands.insert(std::move(node)).position;
        }

        /// Wakes `waitIdle` if no key has a strand. Holds the lock.
        void notifyIfIdleLocked() noexcept
        {
#if CORE_CPP_ASYNC_HAS_THREADS
            if (_strands.empty())
                _idle.notify_all();
#endif
        }

        using StrandMap = std::unordered_map<Key, std::shared_ptr<KeyStrandType>, Hash, KeyEqual>;
        using NodeType = StrandMap::node_type;

        /// How many retired strands, and map nodes, are kept for reuse at most. A key that goes idle
        /// and busy again -- or a new key after an old one went idle -- then costs no allocation
        /// for its strand: its pump's frame, its queue's room and its map node are the kept ones.
        static constexpr std::size_t SpareStrands = 32;

        IExecutor& _base;
        StrandOptions _options;
        KeyedAroundTask<Key> _aroundTask;

        mutable std::mutex _mutex; ///< Guards everything below; taken before any strand's own.
#if CORE_CPP_ASYNC_HAS_THREADS
        std::condition_variable _idle; ///< Signalled when the last strand retires.
#endif
        StrandMap _strands;
        /// Retired strands kept for reuse, each with its pump waiting. One that a parked coroutine
        /// still references is not reused until that reference is gone.
        std::vector<std::shared_ptr<KeyStrandType>> _spareStrands;
        std::vector<NodeType> _spareNodes; ///< Map nodes kept for reuse, empty.
        bool _closed { false };
        bool _sealed { false }; ///< The `try` members refuse; `submit` and `post` are admitted.
    };

} // namespace detail

/// One @c Strand per key over a shared base executor: work for one key runs serially and in
/// order, and work for different keys runs concurrently where the base has the threads.
///
/// For state partitioned by a key -- a model instance, a connection, a session -- where one strand
/// would serialise everything and a strand per object would have to be created, owned and
/// destroyed by hand.
///
/// **Made lazily, reclaimed when idle.** A key has a strand only while it has work queued or
/// running: the submit that finds none makes one, and the strand retires itself when its queue
/// runs dry, so a program keyed by connection holds strands for its busy connections, not for every
/// connection it ever had. A coroutine that parked on something another thread completes while
/// its key was reclaimed comes back to the key -- to its current strand, or a new one -- never to a
/// second strand beside it. Up to 32 reclaimed strands are kept, with their pumps, queue room and
/// map nodes, for the next key that needs one: in the steady state a post to an idle key allocates
/// the call alone, and a submit nothing. **A kept strand and a kept map node each still hold the
/// key they last served**, until they serve another or these strands are closed: so up to 64 keys
/// outlive their work. A key should be cheap to keep and own nothing heavy -- an id, not the object
/// it names.
///
/// Every member is callable from any thread. What `Strand` says about a task, the current executor,
/// a throw out of `resume()`, posted calls, the `try` members and destruction holds for each key's
/// strand; `runningHere(key)` and `runningAnyHere()` are its queries. The around-task hook is a
/// `KeyedAroundTask`, given the key. The base must outlive this object and run what it queues.
///
/// **Single-threaded WebAssembly.** `waitIdle()` does not exist there: nothing else can finish the
/// work, and a blocking wait is not allowed. Destroying or closing these strands drops what is
/// queued without waiting, since nothing else can be running; a host that wants the work run
/// first pumps its base until `idle()`.
///
/// @tparam Key The key type: copyable and copy-assignable, hashable by @p Hash, compared by
///         @p KeyEqual.
/// @tparam Hash Hashes a key.
/// @tparam KeyEqual Compares two keys; default-constructed wherever it is used.
template <typename Key, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>>
class KeyedStrands final
{
  public:
    /// @param base Where every key's strand runs. Must outlive this object.
    /// @param options How each key's strand shares the base. Its `aroundTask` must be unset.
    /// @param aroundTask Called around every task, with its key.
    explicit KeyedStrands(IExecutor& base, StrandOptions options = {}, KeyedAroundTask<Key> aroundTask = {}):
        _registry(std::make_shared<Registry>(base, options, aroundTask))
    {
        assert(!options.aroundTask
               && "KeyedStrands takes its around-task hook as a KeyedAroundTask, which is given the key");
    }

    KeyedStrands(KeyedStrands const&) = delete;
    KeyedStrands(KeyedStrands&&) = delete;
    KeyedStrands& operator=(KeyedStrands const&) = delete;
    KeyedStrands& operator=(KeyedStrands&&) = delete;

    /// Closes every key's strand, as `~Strand` does: queued work is dropped, freeing what nobody
    /// owns, and a task running on another thread is waited for. Work that arrives afterwards --
    /// a coroutine that parked on one of these strands and is resumed later -- is dropped too. Called
    /// from inside a task of one of these strands, it does not wait for that task, as `~Strand` does not.
    ~KeyedStrands() { _registry->close(); }

    /// Awaitable that continues the awaiting coroutine on one key's strand.
    class ResumeOnKey final
    {
      public:
        /// @param strands The strands. @param key The key whose strand to continue on.
        ResumeOnKey(KeyedStrands& strands, Key key): _strands(&strands), _key(std::move(key)) {}

        /// @return False: always suspend, so the resumption happens on the strand.
        [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

        /// Queues the awaiting coroutine on the key's strand.
        /// @tparam Promise The awaiting coroutine's promise type.
        /// @param handle The suspended coroutine.
        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle)
        {
            _strands->submit(_key, detail::parkedWorkFor(handle));
        }

        void await_resume() const noexcept {}

      private:
        KeyedStrands* _strands;
        Key _key;
    };

    /// Queues @p handle, borrowed, on @p key's strand.
    /// @param key The key. @param handle The coroutine to resume there.
    void submit(Key const& key, std::coroutine_handle<> handle)
    {
        submit(key, ParkedWork { .resume = handle });
    }

    /// Queues @p work on @p key's strand, holding its claim until it runs.
    /// @param key The key. @param work The coroutine to resume there, and its claim on the chain.
    void submit(Key const& key, ParkedWork work) { _registry->submit(key, std::move(work)); }

    /// @param key The key whose strand to continue on.
    /// @return An awaitable: `co_await strands.resumeOn(key)` hops onto that key's strand.
    [[nodiscard]] ResumeOnKey resumeOn(Key key) { return ResumeOnKey { *this, std::move(key) }; }

    /// @param key The key to ask about.
    /// @return Whether the calling thread is inside a task of @p key's strand.
    [[nodiscard]] bool runningHere(Key const& key) const noexcept
    {
        return ExecutorScope::anyInForce([this, &key](ExecutorScope const& scope) noexcept {
            return scope.family() == _registry.get()
                   && KeyEqual {}(static_cast<KeyStrand const&>(scope.executor()).key(), key);
        });
    }

    /// @return Whether the calling thread is inside a task of any key's strand.
    [[nodiscard]] bool runningAnyHere() const noexcept
    {
        return ExecutorScope::anyInForce(
            [this](ExecutorScope const& scope) noexcept { return scope.family() == _registry.get(); });
    }

    /// @return How many keys have a strand right now: those with work queued or running. Racy by
    ///         nature; for tests and metrics.
    [[nodiscard]] std::size_t size() const { return _registry->size(); }

    /// Queues @p fn, a callable, on @p key's strand to run as one task. Callable from any thread.
    ///
    /// As `Strand::post`: held by value in one allocation, admitted after a seal, and dropped
    /// uncalled once these strands are closed. @p fn is moved in under the registry's lock: its move
    /// constructor must not submit to any key of these strands.
    /// @param key The key. @param fn The callable, called with no arguments.
    template <typename F>
        requires std::invocable<std::decay_t<F>&> && std::constructible_from<std::decay_t<F>, F>
    void post(Key const& key, F&& fn)
    {
        std::ignore = _registry->offerCall(key, std::forward<F>(fn), detail::WhenSealed::Admit);
    }

    /// Queues @p fn on @p key's strand, unless these strands are closed or sealed. As
    /// `Strand::tryPost`, and
    /// @p fn is moved in under the registry's lock: its move constructor must not submit to any key
    /// of these strands.
    /// @param key The key. @param fn The callable; moved from only where this returns true.
    /// @return Whether it was queued.
    template <typename F>
        requires std::invocable<F&> && std::move_constructible<F>
    [[nodiscard]] bool tryPost(Key const& key, F& fn)
    {
        return _registry->offerCall(key, std::move(fn), detail::WhenSealed::Refuse);
    }

    /// Queues @p handle, borrowed, on @p key's strand, unless these strands are closed or sealed.
    /// @param key The key. @param handle The coroutine to resume there.
    /// @return Whether it was queued.
    [[nodiscard]] bool trySubmit(Key const& key, std::coroutine_handle<> handle)
    {
        auto work = ParkedWork { .resume = handle };
        return _registry->trySubmit(key, work);
    }

    /// Queues @p work on @p key's strand, unless these strands are closed or sealed.
    /// @param key The key. @param work The coroutine and its claim; moved from only where this
    ///        returns true.
    /// @return Whether it was queued.
    [[nodiscard]] bool trySubmit(Key const& key, ParkedWork& work) { return _registry->trySubmit(key, work); }

    /// Closes every key's strand, as the destructor does: queued work is dropped, a task running on
    /// another thread is waited for, and whatever arrives later -- a coroutine that parked on one
    /// of these strands included -- is dropped, or refused by the `try` members. Idempotent; the
    /// destructor calls it.
    void close() { _registry->close(); }

    /// Closes the offer door for every key, and keeps running what is queued and what comes back:
    /// as `Strand::seal`, for all of them at once.
    ///
    /// `tryPost` and `trySubmit` are refused for every key, one with no strand included, and no
    /// strand is made for it. `post` and `submit` are still admitted until `close()`, a key with
    /// no strand included -- a coroutine parked on a key's strand comes back that way -- and a
    /// strand made for one is not kept for reuse once it runs dry; no kept strand is handed out
    /// after the seal. `idle()` and `waitIdle()` then mean nothing queued and nothing running on
    /// any key: a coroutine suspended off its key's strand is invisible to both, so a consumer
    /// counts its own in-flight work and waits for it too before `close()`. The teardown that loses
    /// nothing is `seal()`; then that count and `waitIdle()` -- `post` being new work too, a producer
    /// that posts stops, or offers through `tryPost`, before it -- or, on the single-threaded
    /// WebAssembly build, the base run until both say done; then `close()`. Idempotent.
    void seal() { _registry->seal(); }

    /// @return Whether no key has work queued or running: what a single-threaded host pumps its
    ///         base until before it destroys these strands. Racy by nature where other threads
    ///         submit.
    [[nodiscard]] bool idle() const { return _registry->idle(); }

#if CORE_CPP_ASYNC_HAS_THREADS
    /// Blocks until no key has work queued or running, including work submitted while it waits.
    ///
    /// Not from inside a task of these strands, which would wait for itself: asserted. Not in the
    /// single-threaded WebAssembly build either, where there is no other thread to finish the work
    /// and a blocking wait is not allowed; that build does not declare it.
    void waitIdle()
    {
        assert(!runningAnyHere()
               && "KeyedStrands::waitIdle called from one of its own tasks would wait for itself");
        _registry->waitIdle();
    }
#endif

  private:
    using Registry = detail::KeyedStrandsRegistry<Key, Hash, KeyEqual>;
    using KeyStrand = detail::KeyStrand<Key, Hash, KeyEqual>;

    std::shared_ptr<Registry> _registry;
};

} // namespace core::async
