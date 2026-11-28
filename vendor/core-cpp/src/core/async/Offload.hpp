// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `offload` — run a blocking call on another executor and come back.

#include <core/async/Cancellation.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>

#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace core::async
{

/// Runs @p work on @p blocking and resumes on @p home.
///
/// The awaiting coroutine hops to @p blocking, calls @p work there, and hops back to @p home before
/// it sees the result, so a caller on a loop never runs the blocking call itself and never resumes
/// on a pool thread. Nothing here owns a thread: both executors are the caller's.
///
/// **The stop token is read once, before the hop.** A stop already requested throws
/// @c OperationCancelled and @p work never runs. Once @p work has run, its result is returned
/// whatever the token says by then: a write that completed, or a handle that was opened, is never
/// dropped on the floor because a stop arrived while it was in flight.
///
/// The executors are pointers because a coroutine's parameters are copied into its frame, and a
/// reference parameter would name storage the caller may destroy while this one is suspended.
///
/// @tparam F A callable taking no arguments; its result must not be a reference.
/// @param home Where the caller resumes. Must outlive the returned task.
/// @param blocking Where @p work runs. Must outlive the returned task.
/// @param work The blocking call, moved into the coroutine frame.
/// @return @p work's result, on @p home.
/// @throws OperationCancelled Where the flow's stop was requested before the hop. What @p work
///         throws is rethrown, on @p home.
template <typename F>
[[nodiscard]] Task<std::invoke_result_t<F>> offload(IExecutor* home, IExecutor* blocking, F work)
{
    using Result = std::invoke_result_t<F>;
    static_assert(!std::is_reference_v<Result>, "offload: work must return a value, not a reference");

    auto const token = co_await thisCoroStopToken();
    if (token.stop_requested())
        throw OperationCancelled {};

    co_await ResumeOn { *blocking };
    auto failure = std::exception_ptr {};
    if constexpr (std::is_void_v<Result>)
    {
        try
        {
            std::invoke(work);
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        co_await ResumeOn { *home };
        if (failure)
            std::rethrow_exception(failure);
    }
    else
    {
        auto result = std::optional<Result> {};
        try
        {
            result.emplace(std::invoke(work));
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        co_await ResumeOn { *home };
        if (failure)
            std::rethrow_exception(failure);
        co_return std::move(*result);
    }
}

} // namespace core::async
