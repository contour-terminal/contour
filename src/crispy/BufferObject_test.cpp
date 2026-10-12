// SPDX-License-Identifier: Apache-2.0
#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

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
