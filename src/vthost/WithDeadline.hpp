// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Races a piece of work against a deadline task: what the ssh transport's connect and an attach's budget
/// both are, apart from how their deadlines are worked out.

#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace vthost
{

/// @param work What is waited for.
/// @param deadline Returns when the time is up; cancelled if @p work finishes first.
/// @return What @p work returned; nullopt when @p deadline returned first.
template <typename T>
core::async::Task<std::optional<T>> withDeadline(core::async::Task<T> work, core::async::Task<void> deadline)
{
    auto slot = std::make_shared<std::optional<T>>();
    auto const capture = [](core::async::Task<T> task,
                            std::shared_ptr<std::optional<T>> into) -> core::async::Task<void> {
        *into = co_await std::move(task);
    };
    auto tasks = std::vector<core::async::Task<void>> {};
    tasks.push_back(capture(std::move(work), slot));
    tasks.push_back(std::move(deadline));
    if (co_await core::async::whenAny(std::move(tasks)) == 0)
        co_return std::move(*slot);
    co_return std::nullopt;
}

} // namespace vthost
