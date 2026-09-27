// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ParkedWork` — a coroutine handed to an executor, and what that executor may free if it
/// never resumes it.
///
/// An executor that queues coroutines has to answer one question its interface does not ask:
/// what happens to work still queued when the executor goes away. Resuming it is not an
/// alternative — a bounded wait re-parks and the drain spins — and freeing it wholesale is a
/// double free for every caller whose `Task` object still owns the frame. So the answer travels
/// with the park, as @c ParkedWork::abandon, which `core::async::detail::parkedWorkFor` fills in
/// exactly where the await chain bottoms out in a @c DetachedTask: the one coroutine shape in
/// this module that nobody owns.
///
/// Origin: [fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025),
/// [fastcached#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054).

#include <core/async/Awaitable.hpp>
#include <core/async/DetachedTask.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace core::async
{

namespace detail
{

    /// What the parks of one chain agree on: which frame to free, and whether it is still theirs.
    ///
    /// **A fan-out hands the SAME root to every child**, so an executor can hold N parks naming one
    /// frame; freeing it per park frees it N times. The count here is what makes that one free: the
    /// last claim to go, and only that one, destroys the root.
    ///
    /// It does not free the frame from its own destructor, which is why the root's promise may hold it
    /// strongly without a cycle. And it outlives the frame it frees, because the claim that frees it
    /// is still holding a reference while it does.
    class AbandonState final
    {
      public:
        /// @param root The chain root this state answers for.
        explicit AbandonState(std::coroutine_handle<> root) noexcept: _root(root) {}

        AbandonState(AbandonState const&) = delete;
        AbandonState(AbandonState&&) = delete;
        AbandonState& operator=(AbandonState const&) = delete;
        AbandonState& operator=(AbandonState&&) = delete;
        ~AbandonState() = default;

        /// Records one more park on this chain, for a claim copied from one that already exists.
        ///
        /// Relaxed, and safe for the same reason `shared_ptr`'s increment is: a copy is only made
        /// by a thread that already holds a claim, so the state is already visible to it. It never
        /// touches @c ArmedBit, so it cannot manufacture the state `claimAndArm` exists to prevent.
        void claim() noexcept { _word.fetch_add(1, std::memory_order_relaxed); }

        /// Takes a claim for a NEW park and arms the chain, **in one step**.
        ///
        /// The two halves were once two stores, and that was a use-after-free: `rearm()` published
        /// *armed* before the claim published *counted*, so a concurrent `release()` could observe
        /// a state that never existed as a whole -- armed by this park, count zero because this
        /// park had not been counted yet -- conclude the chain was abandoned, and destroy a frame
        /// that was being parked. Measured at 15 crashes in 640 runs with the window widened by
        /// 200us, against 0 in 640 with the same delay moved one line earlier.
        ///
        /// One compare-exchange makes *armed* unobservable without the claim that accompanies it.
        void claimAndArm() noexcept
        {
            auto expected = _word.load(std::memory_order_relaxed);
            while (!_word.compare_exchange_weak(
                expected, (expected + 1) | ArmedBit, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
            }
        }

        /// Gives up one park's claim, freeing the chain where this was the last claim on an armed
        /// chain -- decided, and claimed, in one step.
        ///
        /// The same compare-exchange that takes the count to zero also clears @c ArmedBit, so the
        /// right to destroy is claimed rather than merely observed and exactly one thread can hold
        /// it. Acquire-release, so that thread sees everything every other claim released.
        void release() noexcept
        {
            auto expected = _word.load(std::memory_order_relaxed);
            auto takesTheRoot = false;
            auto desired = std::uint64_t {};
            do
            {
                auto const next = expected - 1;
                takesTheRoot = (next & CountMask) == 0 && (next & ArmedBit) != 0;
                desired = takesTheRoot ? (next & ~ArmedBit) : next;
            } while (!_word.compare_exchange_weak(
                expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed));

            if (takesTheRoot)
                if (auto const root = std::exchange(_root, {}))
                    root.destroy();
        }

        /// Gives the chain back, for every holder at once: nothing frees it afterwards.
        ///
        /// Resuming ONE park of a chain hands the whole chain back to whoever owns it, so a sibling
        /// still queued must not free what is running again. Arming again is `claimAndArm`'s job
        /// and never a step of its own -- see the note there for what a bare re-arm cost.
        void disarm() noexcept { _word.fetch_and(~ArmedBit, std::memory_order_release); }

        /// Gives up one park's claim AND gives the chain back, in one step: `disarm()` followed by a
        /// `release()` that therefore cannot free anything, for the price of one atomic operation
        /// rather than two.
        ///
        /// It is what a park does when it is resumed or taken back -- the chain is running again, or
        /// is about to be handed to whoever took it -- so it never frees the root. The count must
        /// include this claim.
        void releaseDisarmed() noexcept
        {
            auto expected = _word.load(std::memory_order_relaxed);
            while (!_word.compare_exchange_weak(
                expected, (expected - 1) & ~ArmedBit, std::memory_order_release, std::memory_order_relaxed))
            {
            }
        }

        /// @return Whether the chain is still this state's to free.
        [[nodiscard]] bool armed() const noexcept
        {
            return (_word.load(std::memory_order_acquire) & ArmedBit) != 0;
        }

      private:
        /// The park count and the armed flag, in one word because they answer one question
        /// together. Bit 63 is armed; the rest is the count.
        static constexpr std::uint64_t ArmedBit = std::uint64_t { 1 } << 63U;
        static constexpr std::uint64_t CountMask = ~ArmedBit;

        std::coroutine_handle<> _root;
        std::atomic<std::uint64_t> _word { ArmedBit };
    };

    /// One park's claim on the root of a chain nobody owns: copyable, and the last one to go frees it.
    class AbandonClaim final
    {
      public:
        AbandonClaim() noexcept = default;

        /// @param state The chain's shared state, or empty for a chain somebody owns.
        explicit AbandonClaim(std::shared_ptr<AbandonState> state) noexcept: _state(std::move(state))
        {
            if (_state)
                _state->claim();
        }

        /// Tag for the constructor that takes over a claim already counted by `claimAndArm`.
        struct Adopt
        {
        };

        /// Takes over a claim the caller has already counted, rather than counting another.
        ///
        /// `claimOn` has to count and arm in one atomic step, so it does that on the state and
        /// hands the result here; a constructor that claimed again would count the same park twice.
        /// @param state The chain's shared state; its count already includes this claim.
        AbandonClaim(Adopt /*adopt*/, std::shared_ptr<AbandonState> state) noexcept: _state(std::move(state))
        {
        }

        AbandonClaim(AbandonClaim const& other) noexcept: _state(other._state)
        {
            if (_state)
                _state->claim();
        }

        AbandonClaim(AbandonClaim&& other) noexcept: _state(std::exchange(other._state, {})) {}

        /// By value, so one body serves copy and move assignment.
        /// @param other What to take over.
        /// @return This claim.
        AbandonClaim& operator=(AbandonClaim other) noexcept
        {
            _state.swap(other._state);
            return *this;
        }

        /// A null check where the claim is empty, which it is for every park with no detached chain
        /// behind it -- the common case, destroyed several times per parked operation -- and the
        /// release out of line only where there is one to make.
        ~AbandonClaim()
        {
            if (_state)
                releaseClaim();
        }

        /// Gives the chain back, for every claim on it. See @c AbandonState::disarm().
        void disarm() const noexcept
        {
            if (_state)
                _state->disarm();
        }

        /// @return Whether this claim names a chain at all.
        explicit operator bool() const noexcept { return static_cast<bool>(_state); }

        /// @return Whether the chain is still the claims' to free. For tests.
        [[nodiscard]] bool armed() const noexcept { return _state && _state->armed(); }

        /// @return Whether both claims are on the same chain. For tests.
        [[nodiscard]] bool sameChainAs(AbandonClaim const& other) const noexcept
        {
            return _state == other._state;
        }

        /// Gives this claim up now rather than at destruction, which frees the chain if it was the
        /// last one; empty afterwards.
        void reset() noexcept
        {
            if (_state)
                releaseClaim();
        }

        /// Gives this claim up and takes over @p other's, leaving @p other empty.
        /// @param other The claim to take over.
        void adopt(AbandonClaim& other) noexcept
        {
            reset();
            _state = std::exchange(other._state, {});
        }

      private:
        /// Gives up this claim. A function of its own, so the destructor an empty claim runs -- a null
        /// check -- is small enough to inline where it is called.
        void releaseClaim() noexcept
        {
            _state->release();
            _state.reset();
        }

        std::shared_ptr<AbandonState> _state;
    };

} // namespace detail

/// A coroutine parked on an executor: the handle to resume, and -- only when this chain belongs
/// to nobody -- a claim on the frame that executor may free if it never resumes it.
///
/// **The two halves are different questions, and the second has a default that is safe.**
/// `IExecutor::submit(std::coroutine_handle<>)` BORROWS: the contract is "resume this", and the
/// caller guarantees the frame stays alive until it does. That is why an executor may not simply
/// destroy what it holds at teardown -- a caller parking a handle whose frame a live `Task` owns
/// would be double-freed rather than have a leak fixed.
///
/// **It is the chain's ROOT, never the parked frame itself, and that distinction is the whole
/// point.** Destroying the parked frame runs its own destructors and stops there: whoever awaits
/// it through a `Task::Awaiter` is left holding a dangling handle, is itself unreachable, and the
/// chain goes on leaking -- which is how fastcached measured a four-allocation leak going to three
/// with LeakSanitizer still red. Destroying the root frees all of it, because ownership in a
/// `Task` chain runs downward: each frame's awaiter owns the frame it awaits.
///
/// **The claim is refcounted, because a chain can have more than one live park.** A `whenAll` or
/// `whenAny` gives the same root to every child, so two children that park hand the executor two
/// entries naming one frame. The invariant is not *one park per root* -- it is **one FREE per
/// root**, and the claim is what counts the parks so the last one performs it (controller ruling
/// R97).
struct ParkedWork
{
    std::coroutine_handle<> resume {}; ///< The coroutine to resume. Never owned by the executor.
    detail::AbandonClaim
        abandon {}; ///< The chain to free if it is never resumed; empty when owned elsewhere.
};

namespace detail
{

    /// The frame an executor may free if it never resumes @p handle, or an empty handle where
    /// something else owns this chain.
    ///
    /// Three answers, and the third is what makes this safe to ask at all:
    ///
    /// - a @c DetachedTask is owned by nobody, so it is its own root;
    /// - a promise that carries an @c unownedRoot answers with it — the root of the chain rather
    ///   than this frame, because freeing the frame an executor happens to hold would leave
    ///   whoever awaits it unreachable and still leaked;
    /// - anything else, including the type-erased `std::coroutine_handle<>` (@p Promise deduces
    ///   to `void`), answers *not mine*, which is the borrowing behaviour.
    ///
    /// @tparam Promise The parking coroutine's promise type, as the compiler passes it to
    ///         `await_suspend`.
    /// @param handle The coroutine about to park.
    /// @return The chain root to free on abandonment, or an empty handle.
    template <typename Promise>
    [[nodiscard]] std::coroutine_handle<> unownedRootOf(std::coroutine_handle<Promise> handle) noexcept
    {
        if constexpr (std::is_same_v<Promise, DetachedTask::promise_type>)
            return handle;
        else if constexpr (CarriesUnownedRoot<Promise>)
            return handle.promise().unownedRoot;
        else
            return {};
    }

    /// @param root A chain root, as @c unownedRootOf answers: always a @c DetachedTask, whose handle
    ///        therefore converts back to the typed one that names the promise.
    /// @return The root's promise, which holds the chain's @c AbandonState.
    [[nodiscard]] inline DetachedTask::promise_type& rootPromiseOf(std::coroutine_handle<> root) noexcept
    {
        return std::coroutine_handle<DetachedTask::promise_type>::from_address(root.address()).promise();
    }

    /// The state every park of @p root's chain shares, made by the first park and found by every
    /// later one.
    ///
    /// The state lives in the root's own promise, so the sharing needs no registry. `std::call_once`
    /// rather than a null check, because a fan-out's children can park on two threads at once and
    /// two null checks make two states, which is the very thing this exists to prevent.
    /// @param root The chain root; must not be empty.
    /// @return The state, as the root's promise holds it.
    [[nodiscard]] inline std::shared_ptr<AbandonState> const& sharedStateOf(std::coroutine_handle<> root)
    {
        auto& promise = rootPromiseOf(root);
        std::call_once(promise.abandonOnce,
                       [&promise, root] { promise.abandonState = std::make_shared<AbandonState>(root); });
        return promise.abandonState;
    }

    /// The claim on @p root, made by the first park of a chain and shared by every later one.
    /// @param root The chain root, or an empty handle for a chain somebody owns.
    /// @return A claim on it, or an empty claim.
    [[nodiscard]] inline AbandonClaim claimOn(std::coroutine_handle<> root)
    {
        if (!root)
            return {};

        auto const& state = sharedStateOf(root);
        // A chain that parks again is one an executor may once more have to free, so a fresh claim
        // undoes whatever an earlier resumption disarmed -- and takes the count with it, in the
        // same atomic step, because a chain that is armed but uncounted reads as abandoned to a
        // concurrent `release()`.
        state->claimAndArm();
        return AbandonClaim { AbandonClaim::Adopt {}, state };
    }

    /// One park's claim on a chain nobody owns, COUNTED in the chain's state like an
    /// @c AbandonClaim but holding no reference to that state: it names the root, and reaches the
    /// state through the root's promise.
    ///
    /// **Why it exists: it is the claim of the hottest park there is.** Every socket operation that
    /// parks is completed by its owner and queued for the loop's drain step (G2), and for a chain
    /// rooted in a @c DetachedTask -- every connection a server spawns -- that queue entry needs a
    /// claim. An @c AbandonClaim costs five atomic operations over its life: the reference taken
    /// and dropped on the state, the arm, the disarm at the resume and the release after it. This
    /// costs two: the arm and count in one step when the park is made, and @c giveBack's disarm
    /// and uncount in one step when it is resumed or taken back.
    ///
    /// **What makes the missing reference safe is where it may be held**, and it is a narrower
    /// place than an @c AbandonClaim's: a queue whose entry the parked frame TAKES BACK if it is
    /// destroyed before the queue reaches it -- @c EventLoop's ready queue, which
    /// @c net::ResultAwaitable's destructor searches. The root's promise, which holds the state,
    /// is then alive for as long as this claim is:
    ///
    /// - no other claim can free the root, because this one is counted;
    /// - the root cannot end normally, because the frame this claim parks is suspended inside it;
    /// - a chain destroyed by its owner destroys the parked frame's locals -- the awaitable that
    ///   takes the entry back, and this claim with it -- before the root's promise;
    /// - and @c giveBack runs BEFORE the resume, not after it as @c Parked::resume releases its
    ///   claim: the resume may run the chain to its end, which frees the root and the state with
    ///   it. Giving back first loses nothing, because a resumed park disarms before resuming in
    ///   both, so no other claim could have freed the chain in between.
    ///
    /// The one path that can FREE the root -- the last claim on an armed chain, dropped by a loop
    /// being torn down -- takes a reference for the length of that release, since the state goes
    /// with the root's promise inside it.
    class CountedClaim final
    {
      public:
        CountedClaim() noexcept = default;

        /// Claims @p root for one park: counts the park and arms the chain in one step, as
        /// @c claimOn does.
        /// @param root The chain root, or an empty handle for a chain somebody owns.
        /// @return The claim, or an empty one.
        [[nodiscard]] static CountedClaim on(std::coroutine_handle<> root)
        {
            if (!root)
                return {};
            sharedStateOf(root)->claimAndArm();
            return CountedClaim { root };
        }

        CountedClaim(CountedClaim const&) = delete;
        CountedClaim& operator=(CountedClaim const&) = delete;

        CountedClaim(CountedClaim&& other) noexcept: _root(std::exchange(other._root, {})) {}

        CountedClaim& operator=(CountedClaim&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                _root = std::exchange(other._root, {});
            }
            return *this;
        }

        /// Releases the claim, freeing the chain if it was the last one on an armed chain.
        ~CountedClaim()
        {
            if (_root)
                reset();
        }

        /// @return Whether this claim names a chain at all.
        explicit operator bool() const noexcept { return static_cast<bool>(_root); }

        /// Gives the chain back and this claim up, in one atomic step: what a park does when it is
        /// resumed or taken back. Never frees. Empty afterwards.
        void giveBack() noexcept
        {
            if (auto const root = std::exchange(_root, {}))
                rootPromiseOf(root).abandonState->releaseDisarmed();
        }

        /// Gives this claim up now, which frees the chain if it was the last one on an armed chain;
        /// empty afterwards.
        void reset() noexcept
        {
            auto const root = std::exchange(_root, {});
            if (!root)
                return;
            // Held for the length of the call: freeing the root destroys its promise, and with it
            // the promise's reference, which may be the state's last.
            auto const state = rootPromiseOf(root).abandonState;
            state->release();
        }

      private:
        /// @param root The chain root, whose state already counts this claim.
        explicit CountedClaim(std::coroutine_handle<> root) noexcept: _root(root) {}

        std::coroutine_handle<> _root;
    };

    /// How a coroutine parks itself on an @c IExecutor.
    ///
    /// The one place the ownership question is answered, so no awaitable has to decide it and
    /// none can get it wrong by omission.
    /// @tparam Promise The parking coroutine's promise type.
    /// @param handle The coroutine about to park.
    /// @return The handle to resume, paired with a claim on the chain root to free if it is not.
    template <typename Promise>
    [[nodiscard]] ParkedWork parkedWorkFor(std::coroutine_handle<Promise> handle)
    {
        return ParkedWork { .resume = handle, .abandon = claimOn(unownedRootOf(handle)) };
    }

    /// One entry in an executor's parked-work container, owning @c ParkedWork::abandon for as
    /// long as it sits there.
    ///
    /// **The ownership is folded into the operation rather than called beside it**: `resume()`
    /// disowns and resumes in one expression, so there is no line an executor can forget the
    /// release on, and a container that is simply cleared — at teardown, or by its own destructor
    /// — frees exactly the chains nothing else can.
    class Parked
    {
      public:
        Parked() noexcept = default;

        /// @param work The handle to resume, and the chain root to free if it is not.
        explicit Parked(ParkedWork work) noexcept: _work(std::move(work)) {}

        Parked(Parked const&) = delete;
        Parked& operator=(Parked const&) = delete;

        // The moves, `take` and `abandon` move the two members one by one rather than exchanging a
        // whole `ParkedWork` for a fresh one. That exchange builds a temporary work item and runs a
        // by-value claim assignment and two claim destructors, all on nothing. An entry is moved in
        // and out of a ready queue once per wake, and that was a visible share of it.
        Parked(Parked&& other) noexcept: _work(other.take()) {}

        Parked& operator=(Parked&& other) noexcept
        {
            if (this != &other)
            {
                abandon();
                _work.resume = std::exchange(other._work.resume, {});
                _work.abandon.adopt(other._work.abandon);
            }
            return *this;
        }

        ~Parked() { abandon(); }

        /// @return The handle this entry would resume; empty once it has been taken.
        [[nodiscard]] std::coroutine_handle<> handle() const noexcept { return _work.resume; }

        /// @return True while this entry still holds work.
        explicit operator bool() const noexcept
        {
            return static_cast<bool>(_work.resume) || static_cast<bool>(_work.abandon);
        }

        /// Resumes the parked coroutine, giving the chain back to whoever it belongs to.
        ///
        /// Disowns first and resumes last, so a body that runs to its end and frees its own frame
        /// cannot be freed a second time by this entry going out of scope.
        ///
        /// **A handle that cannot be resumed is FREED here, not dropped.** Taking the work out and
        /// then declining to resume it would discard an owned chain root without destroying it — a
        /// silent leak on the one path this type exists to close. The contract is *resumed or
        /// freed, never neither*.
        void resume()
        {
            auto const work = take();
            if (work.resume && !work.resume.done())
            {
                // Disarmed for EVERY claim on this chain, not only this one: resuming one park
                // gives the whole chain back to whoever owns it, and a sibling still queued must
                // not free what is running again (controller ruling R97).
                work.abandon.disarm();
                work.resume.resume();
                return;
            }
            // Declined, so the claim goes out of scope here and the LAST one to do so frees the
            // chain. Dropping it without freeing would discard an owned chain root -- a silent
            // leak on the one path this type exists to close. The contract is *resumed or freed,
            // never neither*, per chain rather than per park.
        }

        /// Hands the parked work to a caller taking it off this executor.
        ///
        /// After this the caller is the only one who may resume or destroy it, so this entry must
        /// do neither.
        /// @return What was parked here; empty afterwards.
        [[nodiscard]] ParkedWork take() noexcept
        {
            return ParkedWork { .resume = std::exchange(_work.resume, {}),
                                .abandon = std::move(_work.abandon) };
        }

      private:
        /// Gives up this entry's claim on the chain, which frees it if no other park holds one.
        void abandon() noexcept
        {
            _work.resume = {};
            _work.abandon.reset();
        }

        ParkedWork _work {};
    };

} // namespace detail

} // namespace core::async
