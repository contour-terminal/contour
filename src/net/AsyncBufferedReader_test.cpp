// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <vector>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <string_view>

#include <coro/Task.hpp>
#include <net/AsyncBufferedReader.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <net/testing/ScriptedEventSource.h>

using coro::Task;
using net::AsyncBufferedReader;
using net::EventLoop;
using net::NetErrorCode;

namespace
{

/// An ISocket returning scripted byte chunks, one per read() call — the only way
/// to test EXACT read fragmentation (a real socket coalesces buffered sends into
/// one recv). Reads never park; an exhausted script reads as clean EOF.
class FakeSocket final: public net::ISocket
{
  public:
    /// Appends one chunk to be returned by a future read() (split points between
    /// chunks are the fragmentation under test).
    void pushChunk(std::string_view bytes) { _chunks.emplace_back(bytes); }

    Task<net::IoResult> read(std::span<std::byte> buffer) override
    {
        if (_chunks.empty())
            co_return std::size_t { 0 }; // clean EOF
        auto& front = _chunks.front();
        auto const n = std::min(front.size(), buffer.size());
        std::memcpy(buffer.data(), front.data(), n);
        front.erase(0, n);
        if (front.empty())
            _chunks.pop_front();
        co_return n;
    }

    Task<net::IoResult> write(std::span<std::byte const> buffer) override
    {
        co_return buffer.size(); // discarded; the reader never writes
    }

    void close() noexcept override { _closed = true; }

    [[nodiscard]] bool isClosed() const noexcept override { return _closed; }

  private:
    std::deque<std::string> _chunks;
    bool _closed = false;
};

/// Reads one line and reports the outcome through pointers (coroutine params are
/// pointers per cppcoreguidelines-avoid-reference-coroutine-parameters).
Task<void> readOneLine(AsyncBufferedReader* reader, std::string* line, std::optional<net::NetError>* error)
{
    auto result = co_await reader->readLine();
    if (result.has_value())
        *line = std::move(*result);
    else
        *error = result.error();
}

} // namespace

TEST_CASE("readLine assembles lines across fragmented reads without re-scanning", "[net][reader]")
{
    auto fake = FakeSocket {};
    // One 100-byte line + LF, delivered in 2-byte chunks: 51 read() calls. A
    // naive re-scan-from-zero search would examine ~2600 bytes; the scan-offset
    // design examines each byte exactly once.
    auto const line = std::string(100, 'x');
    auto const wire = line + "\n";
    for (std::size_t i = 0; i < wire.size(); i += 2)
        fake.pushChunk(std::string_view { wire }.substr(i, 2));

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto got = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &got, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(got == line);
    REQUIRE(reader.scannedBytes() == wire.size()); // every byte examined exactly once
}

TEST_CASE("readLine strips CRLF and LF alike and keeps a lone CR", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk("crlf\r\nlf\n\r\na\rb\n");

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto lines = std::vector<std::string> {};
    auto collect = [](AsyncBufferedReader* r, std::vector<std::string>* out) -> Task<void> {
        for (auto i = 0; i < 4; ++i)
        {
            auto result = co_await r->readLine();
            REQUIRE(result.has_value());
            out->push_back(std::move(*result));
        }
    };
    loop.blockOn(collect(&reader, &lines));

    REQUIRE(lines == std::vector<std::string> { "crlf", "lf", "", "a\rb" });
}

TEST_CASE("readLine rejects a line exceeding its bound", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk(std::string(32, 'y')); // no terminator, over the 8-byte bound

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake, 8 };

    auto got = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &got, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::MessageTooLarge);
}

TEST_CASE("readLine accepts a line of exactly the bound", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk(std::string(8, 'z'));
    fake.pushChunk("\n"); // terminator arrives in a later chunk

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake, 8 };

    auto got = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &got, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(got == std::string(8, 'z'));
}

TEST_CASE("readLine reports EOF and drops an unterminated tail", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk("complete\n");
    fake.pushChunk("partial"); // the peer dies before sending LF

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto first = std::string {};
    auto firstError = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &first, &firstError));
    REQUIRE(first == "complete");

    auto second = std::string {};
    auto secondError = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &second, &secondError));
    REQUIRE(secondError.has_value());
    REQUIRE(secondError->code == NetErrorCode::Eof);
}

TEST_CASE("readLine works over a real transport through the reactor", "[net][reader][poll]")
{
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto writeThenRead = [](net::ISocket* writer, net::ISocket* peer, std::string* out) -> Task<void> {
        auto const wire = std::string_view { "over the wire\r\n" };
        auto const bytes =
            std::span<std::byte const> { reinterpret_cast<std::byte const*>(wire.data()), wire.size() };
        REQUIRE((co_await writer->write(bytes)).has_value());

        auto reader = AsyncBufferedReader { peer };
        auto result = co_await reader.readLine();
        REQUIRE(result.has_value());
        *out = std::move(*result);
    };

    auto got = std::string {};
    loop.blockOn(writeThenRead(pair->first.get(), pair->second.get(), &got));

    REQUIRE(got == "over the wire");
}
