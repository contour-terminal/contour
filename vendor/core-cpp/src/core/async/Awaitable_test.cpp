// SPDX-License-Identifier: Apache-2.0
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>

#include <coroutine>

using core::async::Awaiter;
using core::async::CarriesUnownedRoot;
using core::async::DetachedTask;
using core::async::HasStopToken;
using core::async::StopToken;
using core::async::Task;
using core::async::ThisCoroStopToken;

// The three concepts of <core/async/Awaitable.hpp>, over the types the module itself offers and
// over the near misses. There is nothing to run here: a concept either holds or it does not, and a
// static_assert says so at compile time. What this file is for is that the contract has one home
// — every templated await_suspend in the module reads the awaiting promise through HasStopToken
// and CarriesUnownedRoot rather than hand-spelling the requirement, and a change to any of them
// fails here rather than silently turning an `if constexpr` into its else branch.

namespace
{

/// The awaiter interface, complete.
struct CompleteAwaiter
{
    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

/// The awaiter interface less its await_resume.
struct AwaiterWithoutResume
{
    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
};

/// An await_ready that answers with something a bool cannot be made of.
struct AwaiterWithUnusableReady
{
    [[nodiscard]] std::coroutine_handle<> await_ready() const noexcept { return {}; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

/// A promise that carries no cancellation token.
struct PromiseWithoutToken
{
};

/// A promise whose stopToken() answers with something that is not a StopToken.
struct PromiseWithWrongToken
{
    [[nodiscard]] int stopToken() const noexcept { return 0; }
};

/// A promise whose unownedRoot is not a coroutine handle: the answer would be unusable, so the
/// concept must not accept it and `unownedRootOf` must fall through to "not mine".
struct PromiseWithWrongRoot
{
    int unownedRoot = 0;
};

} // namespace

static_assert(Awaiter<CompleteAwaiter>);
static_assert(!Awaiter<AwaiterWithoutResume>);
static_assert(!Awaiter<AwaiterWithUnusableReady>);
static_assert(Awaiter<Task<void>::Awaiter>, "co_await-ing a Task<void> drives an awaiter");
static_assert(Awaiter<Task<int>::Awaiter>, "co_await-ing a Task<T> drives an awaiter");
static_assert(Awaiter<ThisCoroStopToken>, "thisCoroStopToken() is awaitable");

static_assert(HasStopToken<Task<void>::PromiseType>, "a Task<void> promise carries its token");
static_assert(HasStopToken<Task<int>::PromiseType>, "a Task<T> promise carries its token");
static_assert(!HasStopToken<PromiseWithoutToken>);
static_assert(!HasStopToken<PromiseWithWrongToken>);
static_assert(!HasStopToken<StopToken>, "the concept is about a promise, not about a token");

static_assert(CarriesUnownedRoot<Task<void>::PromiseType>, "a Task<void> promise carries the chain root");
static_assert(CarriesUnownedRoot<Task<int>::PromiseType>, "a Task<T> promise carries the chain root");
static_assert(!CarriesUnownedRoot<DetachedTask::promise_type>,
              "a detached task IS the root, so it carries no other one: unownedRootOf answers with the "
              "handle itself");
static_assert(!CarriesUnownedRoot<PromiseWithoutToken>);
static_assert(!CarriesUnownedRoot<PromiseWithWrongRoot>);
static_assert(!CarriesUnownedRoot<void>, "the type-erased std::coroutine_handle<> answers 'not mine'");
