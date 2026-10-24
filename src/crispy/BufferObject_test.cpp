// SPDX-License-Identifier: Apache-2.0
#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <ranges>
#include <thread>

TEST_CASE("BufferObject", "[BufferObject]")
{
    // TODO
}

TEST_CASE("BufferObject.advanceHotEndUntil.neverMovesBackwards", "[BufferObject]")
{
    // A parse can find the hot end already past its text, where another writer appended meanwhile:
    // advancing to the end of that text must leave the hot end where the other writer put it.
    auto pool = crispy::BufferObjectPool<char> { 256 };
    auto const buffer = pool.allocateBufferObject();
    auto const* const begin = buffer->data();

    buffer->advanceHotEndUntil(begin + 10);
    REQUIRE(buffer->hotEnd() == begin + 10);

    buffer->advanceHotEndUntil(begin + 4);
    CHECK(buffer->hotEnd() == begin + 10);
}

TEST_CASE("BufferObject.rewindHotEndTo.handsTheTailToTheNextWrite", "[BufferObject]")
{
    auto pool = crispy::BufferObjectPool<char> { 256 };
    auto const buffer = pool.allocateBufferObject();
    auto const* const begin = buffer->data();
    auto const capacity = buffer->bytesAvailable();

    buffer->advanceHotEndUntil(begin + 10);
    buffer->rewindHotEndTo(begin + 4);

    CHECK(buffer->hotEnd() == begin + 4);
    CHECK(buffer->bytesAvailable() == capacity - 4);
}

TEST_CASE("BufferObjectPool.recyclesAcrossThreads", "[BufferObject][threading]")
{
    // A terminal allocates on its reader thread and on the thread that writes to its screen, and a buffer
    // comes back to the pool on whichever thread drops it last. Each thread here holds at most one buffer
    // at a time, so the pool never holds more than the two threads gave back.
    static constexpr auto Rounds = 1000;
    auto pool = crispy::BufferObjectPool<char> { 256 };
    auto dirty = std::atomic<int> { 0 }; // Catch2's assertions are not for other threads
    auto const churn = [&pool, &dirty] {
        for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
        {
            auto const buffer = pool.allocateBufferObject();
            if (buffer->bytesUsed() != 0)
                ++dirty;
            buffer->advance(1);
        }
    };
    {
        auto first = std::jthread { churn };
        auto second = std::jthread { churn };
    }
    CHECK(dirty == 0);
    CHECK(pool.unusedBuffers() <= 2);
}
