// SPDX-License-Identifier: Apache-2.0
//
// `detail::RingQueue`, the event loop's ready queue: order at both ends, across the wrap and across
// growth; an erase from the middle; and the two properties it exists for -- the capacity is kept,
// so a FIFO whose length does not grow stops allocating, and what it holds is released when it is
// erased, cleared or destroyed.
#include <core/net/detail/RingQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

using core::net::detail::RingQueue;

namespace
{

/// @param queue The queue.
/// @return Its elements, front to back.
std::vector<int> contents(RingQueue<std::unique_ptr<int>> const& queue)
{
    auto values = std::vector<int> {};
    for (auto const& element: queue)
        values.push_back(*element);
    return values;
}

/// Moves out of @p from, which a case then inspects.
template <typename T>
T moveOutOf(T& from)
{
    return std::move(from);
}

/// Move-assigns @p from to @p to.
template <typename T>
void moveAssign(T& to, T& from)
{
    to = std::move(from);
}

} // namespace

TEST_CASE("A moved-from RingQueue is empty, and usable", "[net][ringqueue]")
{
    // The implicit move left the source's head and size behind with no slots under them: it
    // answered not empty, and the next push indexed an empty buffer.
    auto source = RingQueue<std::unique_ptr<int>> {};
    source.pushBack(std::make_unique<int>(1));
    source.pushBack(std::make_unique<int>(2));
    auto moved = moveOutOf(source);
    CHECK(contents(moved) == std::vector { 1, 2 });
    REQUIRE(source.empty());
    // `clear()` walked `size()` elements, so it too read slots that were gone.
    source.clear();
    source.pushBack(std::make_unique<int>(3));
    CHECK(contents(source) == std::vector { 3 });

    auto target = RingQueue<std::unique_ptr<int>> {};
    target.pushBack(std::make_unique<int>(9));
    moveAssign(target, moved);
    CHECK(contents(target) == std::vector { 1, 2 });
    CHECK(moved.empty());
}

TEST_CASE("RingQueue keeps FIFO order at both ends, across the wrap and across growth", "[net][ringqueue]")
{
    auto queue = RingQueue<std::unique_ptr<int>> {};
    CHECK(queue.empty());

    // Past the first capacity, with the head moved off slot zero first, so the growth has to
    // unwrap.
    queue.pushBack(std::make_unique<int>(-1));
    CHECK(*queue.takeFront() == -1);
    for (auto const value: std::views::iota(0, 40))
        queue.pushBack(std::make_unique<int>(value));
    queue.pushFront(std::make_unique<int>(-2));
    REQUIRE(queue.size() == 41);
    CHECK(*queue.front() == -2);

    auto expected = std::vector<int> { -2 };
    for (auto const value: std::views::iota(0, 40))
        expected.push_back(value);
    CHECK(contents(queue) == expected);

    auto taken = std::vector<int> {};
    while (!queue.empty())
        taken.push_back(*queue.takeFront());
    CHECK(taken == expected);
}

TEST_CASE("RingQueue erases from the middle and keeps the order of the rest", "[net][ringqueue]")
{
    auto queue = RingQueue<std::unique_ptr<int>> {};
    for (auto const value: std::views::iota(0, 5))
        queue.pushBack(std::make_unique<int>(value));
    auto found = std::ranges::find_if(queue, [](auto const& element) { return *element == 2; });
    REQUIRE(found != queue.end());
    queue.erase(found);
    CHECK(contents(queue) == std::vector<int> { 0, 1, 3, 4 });
    queue.erase(queue.begin());
    CHECK(contents(queue) == std::vector<int> { 1, 3, 4 });
}

TEST_CASE("RingQueue stops growing once a FIFO's length stops growing", "[net][ringqueue]")
{
    // One in, one out, far more often than the capacity: the steady state of a loop's ready queue.
    auto queue = RingQueue<std::unique_ptr<int>> {};
    queue.pushBack(std::make_unique<int>(0));
    auto const capacity = queue.capacity();
    REQUIRE(capacity > 0);
    for (auto const value: std::views::iota(1, 1000))
    {
        queue.pushBack(std::make_unique<int>(value));
        CHECK(*queue.takeFront() == value - 1);
    }
    CHECK(queue.capacity() == capacity);

    queue.clear();
    CHECK(queue.empty());
    CHECK(queue.capacity() == capacity);
}

TEST_CASE("RingQueue releases what it holds when it is erased, cleared or destroyed", "[net][ringqueue]")
{
    auto const held = std::make_shared<int>(7);
    {
        auto queue = RingQueue<std::shared_ptr<int>> {};
        for ([[maybe_unused]] auto const index: std::views::iota(0, 3))
            queue.pushBack(std::shared_ptr<int> { held });
        REQUIRE(held.use_count() == 4);

        queue.erase(queue.begin());
        CHECK(held.use_count() == 3);
        std::ignore = queue.takeFront();
        CHECK(held.use_count() == 2);
        queue.clear();
        CHECK(held.use_count() == 1);

        queue.pushBack(std::shared_ptr<int> { held });
        REQUIRE(held.use_count() == 2);
    }
    CHECK(held.use_count() == 1);
}
