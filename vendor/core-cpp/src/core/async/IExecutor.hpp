// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IExecutor` — somewhere a suspended coroutine can be handed to be resumed.

#include <core/async/ParkedWork.hpp>

#include <coroutine>

namespace core::async
{

/// Somewhere a suspended coroutine can be handed to be resumed.
///
/// The one thing @c ResumeOn needs, split out from the event loop so a thread pool can offer it
/// without pretending to be one. A pool that runs blocking work has no timers, no cancellable
/// deadlines and no clock, and implementing three methods that answer nothing in order to reach a
/// fourth is how an interface stops meaning what it says.
///
/// **Threading contract: `submit` is callable from any thread.** Where the handle resumes is the
/// implementation's business — a loop resumes it on its one loop thread, a pool on whichever of
/// its threads is free — so a coroutine that has awaited its way onto one must not assume it is
/// still where it started.
///
/// **A class deriving from this says `using IExecutor::submit;`.** A derived class that
/// re-declares one overload of a name hides every other overload of it, so every call through the
/// derived type would bind to the borrowing overload — which is how the upstream defect behaved.
/// Here it cannot: **both overloads below are pure**, so a derived class declaring only one hides
/// the other, does not override it, and stays abstract. The `using` is for the reader, and for the
/// day a third overload appears; what an intermediate abstract class could still do is caught by
/// `-Woverloaded-virtual` and by clang-tidy, both errors in this tree. Origin:
/// [fastcached#1041](https://github.com/LASTRADA-Software/fastcached/issues/1041).
class IExecutor
{
  public:
    IExecutor() = default;
    IExecutor(IExecutor const&) = delete;
    IExecutor(IExecutor&&) = delete;
    IExecutor& operator=(IExecutor const&) = delete;
    IExecutor& operator=(IExecutor&&) = delete;
    virtual ~IExecutor() = default;

    /// Posts a coroutine handle for resumption. This BORROWS: the caller guarantees the frame
    /// stays alive until it is resumed.
    /// @param handle Coroutine to resume. Must remain alive until it is.
    virtual void submit(std::coroutine_handle<> handle) = 0;

    /// Posts a coroutine for resumption, saying what may be freed if it never is.
    ///
    /// The overload above borrows, so an executor destroyed with borrowed work still queued can do
    /// nothing about it. This one carries @c ParkedWork::abandon — the root of an await chain
    /// nobody owns — which the executor may free rather than drop.
    ///
    /// Pure rather than defaulted to the borrowing form: an executor that queues work has to state
    /// what it does about work it never runs, and a default would let a new one answer *nothing*
    /// by omission
    /// ([fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
    /// @param work The coroutine to resume, and the chain root to free if it is not.
    virtual void submit(ParkedWork work) = 0;
};

} // namespace core::async
