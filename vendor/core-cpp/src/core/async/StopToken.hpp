// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Cooperative cancellation: `core::async::StopToken`, `StopSource`, `StopCallback<F>` and
/// `NoStopState`.
///
/// They are `std::stop_token`, `std::stop_source`, `std::stop_callback<F>` and `std::nostopstate`
/// where the standard library defines `__cpp_lib_jthread`, and otherwise a core-cpp
/// implementation with the standard semantics. libc++ before 20 has `<stop_token>` only behind
/// `-fexperimental-library` (emsdk 3.1.56's libc++ 17 among them), and a library may not add that
/// flag to its consumers' compiles.
///
/// The fallback's semantics are the standard's:
///  - `request_stop()` returns true exactly once, and that call runs every registered callback
///    once, on the requesting thread, before it returns.
///  - A `StopCallback` constructed on a token whose stop was requested runs its callback in its
///    constructor.
///  - `~StopCallback` deregisters the callback. While the callback runs on another thread, the
///    destructor waits for it to return; called from inside the callback itself, it does not.
///  - `stop_possible()` is false for a token with no stop state, and for one whose sources are
///    all gone without a stop request.
///  - Copies of a token or a source share one stop state.
///  - A callback is invoked as `std::forward<Callback>(callback)()`; one that throws terminates
///    the program.
///
/// Under single-threaded WebAssembly the fallback keeps plain state: no atomics, no lock, no
/// wait.
///
/// The choice is made from `<version>`, included first, so every translation unit makes the same
/// one whatever it included before this header: a `StopToken` is a member of every `Task`
/// promise, and two translation units that disagreed about it would disagree about the layout of
/// every coroutine frame.
///
/// The fallback is always defined, as @c detail::StopTokenFallback, @c detail::StopSourceFallback
/// and @c detail::StopCallbackFallback. Its std-compatible members carry the standard's names
/// (`request_stop`, `stop_requested`, ...), so code compiles against either.

// Define CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK to make StopToken, StopSource and StopCallback the
// fallback even where the standard library has <stop_token>. It has to be defined the same way in
// every translation unit of a program, or they disagree about what a StopToken is; the test
// binary core-cpp-async-fallback-test is built that way.

#include <version>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L \
    && !defined(CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK)
    #define CORE_ASYNC_STOP_TOKEN_IS_STD 1
    #include <stop_token>
#else
    #define CORE_ASYNC_STOP_TOKEN_IS_STD 0
#endif

// Single-threaded WebAssembly has one thread, so the fallback has nothing to synchronise there.
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #define CORE_ASYNC_STOP_TOKEN_HAS_THREADS 1
    #include <atomic>
    #include <condition_variable>
    #include <mutex>
    #include <thread>
#else
    #define CORE_ASYNC_STOP_TOKEN_HAS_THREADS 0
#endif

#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace core::async
{

namespace detail
{

#if CORE_ASYNC_STOP_TOKEN_HAS_THREADS
    /// How the fallback's stop state synchronises where there are threads: a mutex over the
    /// callback list, and a condition variable on which a callback's destructor waits for the
    /// callback to return.
    class StopStateSync
    {
      public:
        /// A held lock of the stop state.
        using Lock = std::unique_lock<std::mutex>;

        /// Identifies a thread.
        using ThreadId = std::thread::id;

        /// A value read without the lock.
        template <typename T>
        using Shared = std::atomic<T>;

        /// @return The stop state's lock, held.
        [[nodiscard]] Lock lock() { return Lock { _mutex }; }

        /// Waits until @p done holds, releasing @p lock meanwhile.
        /// @param lock The held lock, which @p done is evaluated under.
        /// @param done The condition to wait for.
        template <typename Predicate>
        void wait(Lock& lock, Predicate done)
        {
            _callbackReturned.wait(lock, std::move(done));
        }

        /// Wakes every wait.
        void notifyAll() noexcept { _callbackReturned.notify_all(); }

        /// @return The calling thread.
        [[nodiscard]] static ThreadId currentThread() noexcept { return std::this_thread::get_id(); }

      private:
        std::mutex _mutex;
        std::condition_variable _callbackReturned;
    };
#else
    /// How the fallback's stop state synchronises under single-threaded WebAssembly: not at all.
    /// There is one thread, so nothing to lock, nothing to wait for and nothing to make atomic,
    /// and a callback that is running is running on the calling thread.
    class StopStateSync
    {
      public:
        /// Nothing is locked. The attribute keeps a lock that is only held from being reported
        /// as an unused variable.
        struct [[maybe_unused]] Lock
        {
        };

        /// There is one thread.
        struct ThreadId
        {
            [[nodiscard]] bool operator==(ThreadId const&) const noexcept = default;
        };

        /// A value read without the lock.
        template <typename T>
        using Shared = T;

        /// @return Nothing to hold.
        [[nodiscard]] Lock lock() noexcept { return {}; }

        /// Never called with a callback running elsewhere, since there is no elsewhere.
        template <typename Predicate>
        void wait(Lock& /*lock*/, Predicate /*done*/) noexcept
        {
        }

        /// Nothing waits.
        void notifyAll() noexcept {}

        /// @return The one thread.
        [[nodiscard]] static ThreadId currentThread() noexcept { return {}; }
    };
#endif

    class StopState;

    /// A callback as its stop state sees it: a node of the list of callbacks a stop request runs,
    /// and the callback to run. @c StopCallbackFallback is one.
    class StopCallbackNode
    {
      public:
        StopCallbackNode(StopCallbackNode const&) = delete;
        StopCallbackNode(StopCallbackNode&&) = delete;
        StopCallbackNode& operator=(StopCallbackNode const&) = delete;
        StopCallbackNode& operator=(StopCallbackNode&&) = delete;

      protected:
        /// Runs the callback of the node it is given.
        using Invoke = void (*)(StopCallbackNode& node) noexcept;

        /// @param invoke Runs this node's callback.
        explicit StopCallbackNode(Invoke invoke) noexcept: _invoke(invoke) {}

        ~StopCallbackNode() = default;

      private:
        friend class StopState;

        Invoke _invoke;                        ///< The stop state runs it at most once, holding no lock.
        StopCallbackNode* _previous = nullptr; ///< Toward the head of the list; none at the head.
        StopCallbackNode* _next = nullptr;     ///< Toward the tail of the list.
    };

    /// The state a @c StopSourceFallback, its copies, their tokens and the callbacks registered on
    /// those share: whether stop was requested, how many sources there are, and the callbacks a
    /// stop request runs.
    class StopState
    {
      public:
        /// @return Whether stop was requested.
        [[nodiscard]] bool isStopRequested() const noexcept { return _stopRequested; }

        /// @return Whether stop was requested, or a source remains that could request it.
        [[nodiscard]] bool isStopPossible() const noexcept
        {
            // The count first, then the flag. A count of 0 is final: a source is only ever added
            // by copying a live one. Every stop request was made through a source whose removal
            // came after it, and each removal is a read-modify-write of the count, so a read that
            // finds 0 comes after every request (all of it sequentially consistent), and the flag
            // read after it holds the final answer. The other order admits a false: a request and
            // the removal of the last source can both land between the flag's read and the
            // count's, and a stop that was requested reads as impossible.
            return _sourceCount != 0 || _stopRequested;
        }

        /// Counts a new source of this state.
        void addSource() noexcept { ++_sourceCount; }

        /// Counts a source of this state gone.
        void removeSource() noexcept { --_sourceCount; }

        /// Requests stop. The first request runs every registered callback, one at a time, on the
        /// calling thread, holding no lock while a callback runs, and returns once all have run.
        /// @return Whether this was the first request.
        bool requestStop() noexcept
        {
            {
                auto const lock = _sync.lock();
                if (_stopRequested)
                    return false;
                _stopRequested = true;
                _requester = StopStateSync::currentThread();
            }
            while (auto* const node = takeNextCallback())
            {
                node->_invoke(*node); // Its callback may destroy it: node is not touched after this.
                finishCallback();
            }
            return true;
        }

        /// Registers @p node, whose callback the first stop request runs.
        /// @param node The callback, which stays registered until @c deregisterCallback().
        /// @return False when stop was requested already. Nothing is registered then, and the
        ///         caller runs the callback itself.
        [[nodiscard]] bool registerCallback(StopCallbackNode& node) noexcept
        {
            auto const lock = _sync.lock();
            if (_stopRequested)
                return false;
            node._next = _head;
            if (_head)
                _head->_previous = &node;
            _head = &node;
            return true;
        }

        /// Deregisters @p node. If its callback is running on another thread, waits for it to
        /// return; if it is running on this one, that callback is destroying its own node, and
        /// this returns at once.
        ///
        /// A node is known by its address, and a destroyed node's address can be reused, so
        /// only a node that @c registerCallback() accepted may be passed here. Such a node never
        /// shares its address with a running one: a callback runs only once stop was requested,
        /// and from then on every new callback runs in its constructor and is never registered.
        /// @param node A callback that was registered.
        void deregisterCallback(StopCallbackNode& node) noexcept
        {
            auto lock = _sync.lock();
            if (isLinked(node))
            {
                unlink(node);
                return;
            }
            if (_running == &node && _requester != StopStateSync::currentThread())
                _sync.wait(lock, [this, &node] { return _running != &node; });
        }

      private:
        /// Takes the next callback to run off the list, and marks it running.
        /// @return The callback, or null when none is left.
        [[nodiscard]] StopCallbackNode* takeNextCallback() noexcept
        {
            auto const lock = _sync.lock();
            auto* const node = _head;
            if (node)
            {
                unlink(*node);
                _running = node;
            }
            return node;
        }

        /// Marks the running callback as returned, and wakes a destructor waiting for it.
        void finishCallback() noexcept
        {
            {
                auto const lock = _sync.lock();
                _running = nullptr;
            }
            _sync.notifyAll();
        }

        [[nodiscard]] bool isLinked(StopCallbackNode const& node) const noexcept
        {
            return node._previous != nullptr || _head == &node;
        }

        void unlink(StopCallbackNode& node) noexcept
        {
            if (node._previous)
                node._previous->_next = node._next;
            else
                _head = node._next;
            if (node._next)
                node._next->_previous = node._previous;
            node._previous = nullptr;
            node._next = nullptr;
        }

        StopStateSync _sync;
        StopStateSync::Shared<bool> _stopRequested { false };
        StopStateSync::Shared<std::size_t> _sourceCount { 0 };
        StopCallbackNode* _head = nullptr;          ///< The callbacks yet to run; under the lock.
        StopCallbackNode const* _running = nullptr; ///< The callback running now; under the lock.
        StopStateSync::ThreadId _requester {};      ///< The thread that requested stop; under the lock.
    };

    template <typename Callback>
    class StopCallbackFallback;

    /// The tag that constructs a @c StopSourceFallback with no stop state: the fallback
    /// `std::nostopstate_t`.
    struct NoStopStateFallback
    {
        explicit NoStopStateFallback() = default;
    };

    /// Observes a stop request: the fallback `std::stop_token`.
    class StopTokenFallback
    {
      public:
        /// A token with no stop state, whose stop is neither requested nor possible.
        StopTokenFallback() noexcept = default;

        /// @return Whether stop was requested on the state this token observes.
        [[nodiscard]] bool stop_requested() const noexcept { return _state && _state->isStopRequested(); }

        /// @return Whether stop was requested, or a source remains that could request it.
        [[nodiscard]] bool stop_possible() const noexcept { return _state && _state->isStopPossible(); }

        /// Exchanges the stop states of this token and @p other.
        void swap(StopTokenFallback& other) noexcept { _state.swap(other._state); }

        /// Exchanges the stop states of @p a and @p b.
        friend void swap(StopTokenFallback& a, StopTokenFallback& b) noexcept { a.swap(b); }

        /// @return Whether both tokens observe the same stop state, or both none.
        [[nodiscard]] friend bool operator==(StopTokenFallback const&,
                                             StopTokenFallback const&) noexcept = default;

      private:
        friend class StopSourceFallback;

        template <typename Callback>
        friend class StopCallbackFallback;

        explicit StopTokenFallback(std::shared_ptr<StopState> state) noexcept: _state(std::move(state)) {}

        std::shared_ptr<StopState> _state;
    };

    /// Requests stop of every token obtained from it or from a copy of it: the fallback
    /// `std::stop_source`.
    class StopSourceFallback
    {
      public:
        /// A source with a new stop state.
        StopSourceFallback(): _state(std::make_shared<StopState>()) { _state->addSource(); }

        /// A source with no stop state, whose stop is not possible.
        explicit StopSourceFallback(NoStopStateFallback /*tag*/) noexcept {}

        /// A source of the same stop state as @p other.
        StopSourceFallback(StopSourceFallback const& other) noexcept: _state(other._state)
        {
            if (_state)
                _state->addSource();
        }

        /// Takes the stop state of @p other, which is left with none.
        StopSourceFallback(StopSourceFallback&& other) noexcept = default;

        StopSourceFallback& operator=(StopSourceFallback const& other) noexcept
        {
            StopSourceFallback { other }.swap(*this);
            return *this;
        }

        StopSourceFallback& operator=(StopSourceFallback&& other) noexcept
        {
            StopSourceFallback { std::move(other) }.swap(*this);
            return *this;
        }

        ~StopSourceFallback()
        {
            if (_state)
                _state->removeSource();
        }

        /// Requests stop. The first request runs every callback registered on the stop state,
        /// on this thread, and returns once all have run.
        /// @return Whether this was the first request; false too for a source with no stop state.
        bool request_stop() noexcept
        {
            if (!_state)
                return false;
            // A callback may destroy this source; the state it is running on lives on.
            auto const state = _state;
            return state->requestStop();
        }

        /// @return A token observing this source's stop state (none, if it has none).
        [[nodiscard]] StopTokenFallback get_token() const noexcept { return StopTokenFallback { _state }; }

        /// @return Whether stop was requested.
        [[nodiscard]] bool stop_requested() const noexcept { return _state && _state->isStopRequested(); }

        /// @return Whether this source has a stop state.
        [[nodiscard]] bool stop_possible() const noexcept { return _state != nullptr; }

        /// Exchanges the stop states of this source and @p other.
        void swap(StopSourceFallback& other) noexcept { _state.swap(other._state); }

        /// Exchanges the stop states of @p a and @p b.
        friend void swap(StopSourceFallback& a, StopSourceFallback& b) noexcept { a.swap(b); }

        /// @return Whether both sources have the same stop state, or both none.
        [[nodiscard]] friend bool operator==(StopSourceFallback const&,
                                             StopSourceFallback const&) noexcept = default;

      private:
        std::shared_ptr<StopState> _state;
    };

    /// Runs a callback when stop is requested on its token: the fallback `std::stop_callback`.
    /// @tparam Callback The callback's type, invocable with no arguments.
    template <typename Callback>
    class StopCallbackFallback final: private StopCallbackNode
    {
        static_assert(std::invocable<Callback>, "a StopCallback's callback takes no arguments");
        static_assert(std::destructible<Callback>);

      public:
        /// The callback's type, as `std::stop_callback` names it.
        using callback_type = Callback;

        /// Registers @p callback on the stop state of @p token, or runs it now if stop was
        /// requested already. A token with no stop state never runs it.
        /// @param token The token whose stop runs the callback.
        /// @param callback Initialises the callback.
        template <typename Init>
            requires std::constructible_from<Callback, Init>
        explicit StopCallbackFallback(StopTokenFallback const& token, Init&& callback) noexcept(
            std::is_nothrow_constructible_v<Callback, Init>):
            StopCallbackNode(&StopCallbackFallback::invoke),
            _callback(std::forward<Init>(callback)),
            _state(token._state)
        {
            start();
        }

        /// Registers @p callback on the stop state of @p token, or runs it now if stop was
        /// requested already. A token with no stop state never runs it.
        /// @param token The token whose stop runs the callback; it is left with no stop state.
        /// @param callback Initialises the callback.
        template <typename Init>
            requires std::constructible_from<Callback, Init>
        explicit StopCallbackFallback(StopTokenFallback&& token, Init&& callback) noexcept(
            std::is_nothrow_constructible_v<Callback, Init>):
            StopCallbackNode(&StopCallbackFallback::invoke),
            _callback(std::forward<Init>(callback)),
            _state(std::move(token)._state)
        {
            start();
        }

        /// Deregisters the callback, if it was registered. If it is running on another thread,
        /// waits for it to return first; called from inside the callback, returns at once. A
        /// callback that ran in the constructor has nothing to deregister or wait for.
        ~StopCallbackFallback()
        {
            if (_state)
                _state->deregisterCallback(*this);
        }

        StopCallbackFallback(StopCallbackFallback const&) = delete;
        StopCallbackFallback(StopCallbackFallback&&) = delete;
        StopCallbackFallback& operator=(StopCallbackFallback const&) = delete;
        StopCallbackFallback& operator=(StopCallbackFallback&&) = delete;

      private:
        void start() noexcept
        {
            if (_state && !_state->registerCallback(*this))
            {
                // Stop was requested already. The callback runs here and is never registered, so
                // the destructor must not consult the state: another callback at this address
                // may be running there, and waiting for it could deadlock.
                _state.reset();
                invoke(*this);
            }
        }

        /// Runs the callback of @p node, a StopCallbackFallback<Callback>.
        static void invoke(StopCallbackNode& node) noexcept
        {
            std::forward<Callback>(static_cast<StopCallbackFallback&>(node)._callback)();
        }

        Callback _callback;
        std::shared_ptr<StopState> _state;
    };

    template <typename Callback>
    StopCallbackFallback(StopTokenFallback, Callback) -> StopCallbackFallback<Callback>;

} // namespace detail

#if CORE_ASYNC_STOP_TOKEN_IS_STD

/// Observes a stop request. Cheap to copy; copies share the stop state of the source.
using StopToken = std::stop_token;

/// Requests stop of every @c StopToken obtained from it or from a copy of it.
using StopSource = std::stop_source;

/// Runs a callback when stop is requested on its token.
/// @tparam Callback The callback's type, invocable with no arguments.
template <typename Callback>
using StopCallback = std::stop_callback<Callback>;

/// Constructs a @c StopSource with no stop state: `StopSource { NoStopState }`.
inline constexpr std::nostopstate_t NoStopState {};

#else

/// Observes a stop request. Cheap to copy; copies share the stop state of the source.
using StopToken = detail::StopTokenFallback;

/// Requests stop of every @c StopToken obtained from it or from a copy of it.
using StopSource = detail::StopSourceFallback;

/// Runs a callback when stop is requested on its token.
/// @tparam Callback The callback's type, invocable with no arguments.
template <typename Callback>
using StopCallback = detail::StopCallbackFallback<Callback>;

/// Constructs a @c StopSource with no stop state: `StopSource { NoStopState }`.
inline constexpr detail::NoStopStateFallback NoStopState {};

#endif

} // namespace core::async
