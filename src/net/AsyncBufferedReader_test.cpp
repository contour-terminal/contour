// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <coro/Task.hpp>
#include <net/AsyncBufferedReader.hpp>
#include <net/EventLoop.hpp>
#include <net/ISocket.hpp>
#include <net/PollEventSource.hpp>
#include <net/testing/InMemoryTransport.hpp>
#include <net/testing/ScriptedEventSource.hpp>

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

    /// Makes the read AFTER the scripted chunks fail with @p code instead of
    /// reporting a clean EOF — the transport-failure case, distinct from the peer
    /// simply closing.
    void failAfterChunks(net::NetErrorCode code) { _failure = code; }

    Task<net::IoResult> read(std::span<std::byte> buffer) override
    {
        if (_chunks.empty())
        {
            if (_failure.has_value())
                co_return std::unexpected(net::makeNetError(*_failure, 0, "injected failure"));
            co_return std::size_t { 0 }; // clean EOF
        }
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
    std::optional<net::NetErrorCode> _failure;
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

/// Reads up to @p delimiter and reports the outcome through pointers.
Task<void> readOneUntil(AsyncBufferedReader* reader,
                        std::string const* delimiter,
                        std::string* payload,
                        std::optional<net::NetError>* error)
{
    auto result = co_await reader->readUntil(*delimiter);
    if (result.has_value())
        *payload = std::move(*result);
    else
        *error = result.error();
}

/// Reads exactly @p count bytes and reports the outcome through pointers.
Task<void> readOneExactly(AsyncBufferedReader* reader,
                          std::size_t count,
                          std::string* payload,
                          std::optional<net::NetError>* error)
{
    auto result = co_await reader->readExactly(count);
    if (result.has_value())
        *payload = std::move(*result);
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

TEST_CASE("readLine delivers a buffered burst and tracks unconsumed bytes", "[net][reader]")
{
    auto fake = FakeSocket {};
    // Three lines in one read. Split at the literal boundaries so no '\n' abuts a letter (which the
    // spell-checker would read as one glued token); the runtime bytes are still "one\ntwo\nthree\n".
    fake.pushChunk("one\n"
                   "two\n"
                   "three\n");

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto lines = std::vector<std::string> {};
    auto bufferedAfter = std::vector<std::size_t> {};
    auto moreAfter = std::vector<bool> {};
    // Capture nothing, so the coroutine cannot outlive a closure of references
    // (cppcoreguidelines-avoid-capturing-lambda-coroutines): outputs are passed by pointer.
    auto step = [](AsyncBufferedReader* r,
                   std::vector<std::string>* outLines,
                   std::vector<std::size_t>* outBuffered,
                   std::vector<bool>* outMore) -> Task<void> {
        for (auto i = 0; i < 3; ++i)
        {
            auto result = co_await r->readLine();
            REQUIRE(result.has_value());
            outLines->push_back(std::move(*result));
            outBuffered->push_back(r->buffered());
            outMore->push_back(r->hasBufferedLine());
        }
    };
    loop.blockOn(step(&reader, &lines, &bufferedAfter, &moreAfter));

    CHECK(lines == std::vector<std::string> { "one", "two", "three" });
    // Consuming a line advances a cursor, not a front-erase, but buffered() still reports only the
    // undelivered remainder: "two\nthree\n" (10), then "three\n" (6), then nothing (0).
    CHECK(bufferedAfter == std::vector<std::size_t> { 10, 6, 0 });
    // hasBufferedLine sees past the consumed prefix: true while lines remain, false after the last.
    CHECK(moreAfter == std::vector<bool> { true, true, false });
    CHECK(reader.scannedBytes() == 14); // every byte still examined exactly once
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

TEST_CASE("readUntil finds a delimiter split across two reads", "[net][reader]")
{
    // The scan-offset optimization must not skip a delimiter straddling a read
    // boundary: "\r\n\r\n" arrives as "..\r\n" + "\r\n..", so the match begins
    // in bytes already scanned once.
    auto fake = FakeSocket {};
    fake.pushChunk("GET / HTTP/1.1\r\nHost: x\r\n");
    fake.pushChunk("\r\nbody");

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto const delimiter = std::string { "\r\n\r\n" };
    auto head = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneUntil(&reader, &delimiter, &head, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(head == "GET / HTTP/1.1\r\nHost: x");
    REQUIRE(reader.buffered() == 4); // "body" stays for the caller
}

TEST_CASE("readUntil consumes the delimiter and leaves the remainder buffered", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk("head\r\n\r\ntail");

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto const delimiter = std::string { "\r\n\r\n" };
    auto head = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneUntil(&reader, &delimiter, &head, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(head == "head");

    // The delimiter itself is consumed, so a following readExactly sees only "tail".
    auto body = std::string {};
    loop.blockOn(readOneExactly(&reader, 4, &body, &error));
    REQUIRE_FALSE(error.has_value());
    REQUIRE(body == "tail");
}

TEST_CASE("readUntil reports EOF when the delimiter never arrives", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk("no terminator here");

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto const delimiter = std::string { "\r\n\r\n" };
    auto head = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneUntil(&reader, &delimiter, &head, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Eof);
}

TEST_CASE("readUntil rejects a message exceeding the bound", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk(std::string(64, 'x'));
    fake.pushChunk(std::string(64, 'y'));

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake, 32 };

    auto const delimiter = std::string { "\r\n\r\n" };
    auto head = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneUntil(&reader, &delimiter, &head, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::MessageTooLarge);
}

TEST_CASE("readUntil rejects an empty delimiter", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk("anything");

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto const delimiter = std::string {};
    auto head = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneUntil(&reader, &delimiter, &head, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Other);
}

TEST_CASE("readExactly assembles a payload across fragmented reads", "[net][reader]")
{
    auto fake = FakeSocket {};
    auto const payload = std::string(100, 'z');
    for (std::size_t i = 0; i < payload.size(); i += 3)
        fake.pushChunk(std::string_view { payload }.substr(i, 3));

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto got = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneExactly(&reader, payload.size(), &got, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(got == payload);
}

TEST_CASE("readExactly of zero bytes returns empty without reading", "[net][reader]")
{
    auto fake = FakeSocket {}; // no chunks: any read would be EOF

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto got = std::string { "sentinel" };
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneExactly(&reader, 0, &got, &error));

    REQUIRE_FALSE(error.has_value());
    REQUIRE(got.empty());
}

TEST_CASE("readExactly reports EOF rather than delivering a truncated payload", "[net][reader]")
{
    auto fake = FakeSocket {};
    fake.pushChunk("only ten!!"); // 10 bytes, 20 requested

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto got = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneExactly(&reader, 20, &got, &error));

    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::Eof);
    REQUIRE(got.empty());
}

TEST_CASE("readExactly and readLine interleave over one buffer", "[net][reader]")
{
    // The framing pattern HttpServer relies on: a delimited head, then a
    // length-prefixed body, then more lines — all sharing one read cursor.
    auto fake = FakeSocket {};
    fake.pushChunk("header\nAB");
    fake.pushChunk("CDtrailer\n");

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto first = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &first, &error));
    REQUIRE(first == "header");

    auto body = std::string {};
    loop.blockOn(readOneExactly(&reader, 4, &body, &error));
    REQUIRE(body == "ABCD");

    auto last = std::string {};
    loop.blockOn(readOneLine(&reader, &last, &error));
    REQUIRE(last == "trailer");
    REQUIRE_FALSE(error.has_value());
}

TEST_CASE("readUntil finds a delimiter buffered before readLine hit its bound", "[net][reader]")
{
    // The two scanners share _scanOffset but mean different things by it. readLine
    // leaves it at buffer end when it finds no LF; every other exit either matches
    // (leaving it at _consumed) or refills (fill() rebases it), so the one way to
    // observe the contaminated state is readLine's MessageTooLarge return, which
    // exits with _scanOffset past the buffer and does NOT compact. A readUntil on
    // that same connection then resumes scanning past a delimiter it already has.
    auto fake = FakeSocket {};
    fake.pushChunk("xxENDyyyyyy"); // holds "END", but no LF at all

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake, 4 }; // bound smaller than the chunk

    // readLine scans the whole buffer, finds no LF, and refuses to grow past the
    // bound — returning with _scanOffset == buffer.size().
    auto line = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &line, &error));
    REQUIRE(error.has_value());
    REQUIRE(error->code == NetErrorCode::MessageTooLarge);

    // "END" is buffered at index 2. readUntil must find it rather than resuming at
    // the stale offset and reporting EOF for data it is already holding.
    error.reset();
    auto head = std::string {};
    auto const delimiter = std::string { "END" };
    loop.blockOn(readOneUntil(&reader, &delimiter, &head, &error));
    REQUIRE_FALSE(error.has_value());
    REQUIRE(head == "xx");
}

TEST_CASE("readUntil does not skip a delimiter left behind by readLine", "[net][reader]")
{
    // Same contamination, in the order that actually loses data: readLine consumes
    // a line and parks _scanOffset, then readUntil must still see a delimiter that
    // lies in the bytes readLine already scanned past.
    auto fake = FakeSocket {};
    fake.pushChunk("first\nxxENDyy"); // one line, then a delimiter with no LF after it

    auto source = net::testing::ScriptedEventSource {};
    auto loop = EventLoop { source };
    auto reader = AsyncBufferedReader { &fake };

    auto line = std::string {};
    auto error = std::optional<net::NetError> {};
    loop.blockOn(readOneLine(&reader, &line, &error));
    REQUIRE(line == "first");

    // "END" is buffered right now. readUntil must find it rather than reporting
    // EOF because it resumed scanning past it.
    auto head = std::string {};
    auto const delimiter = std::string { "END" };
    loop.blockOn(readOneUntil(&reader, &delimiter, &head, &error));
    REQUIRE_FALSE(error.has_value());
    REQUIRE(head == "xx");
}

TEST_CASE("a transport failure is reported as itself, not as EOF", "[net][reader]")
{
    // A connection reset must not be delivered as a clean end-of-message: the
    // caller would conclude the peer finished sending when the transport actually
    // failed. Each reader shares one refill path, so all three are checked.
    SECTION("readLine")
    {
        auto fake = FakeSocket {};
        fake.pushChunk("no terminator");
        fake.failAfterChunks(NetErrorCode::ConnReset);

        auto source = net::testing::ScriptedEventSource {};
        auto loop = EventLoop { source };
        auto reader = AsyncBufferedReader { &fake };

        auto got = std::string {};
        auto error = std::optional<net::NetError> {};
        loop.blockOn(readOneLine(&reader, &got, &error));

        REQUIRE(error.has_value());
        REQUIRE(error->code == NetErrorCode::ConnReset);
    }

    SECTION("readUntil")
    {
        auto fake = FakeSocket {};
        fake.pushChunk("head");
        fake.failAfterChunks(NetErrorCode::ConnReset);

        auto source = net::testing::ScriptedEventSource {};
        auto loop = EventLoop { source };
        auto reader = AsyncBufferedReader { &fake };

        auto const delimiter = std::string { "\r\n\r\n" };
        auto got = std::string {};
        auto error = std::optional<net::NetError> {};
        loop.blockOn(readOneUntil(&reader, &delimiter, &got, &error));

        REQUIRE(error.has_value());
        REQUIRE(error->code == NetErrorCode::ConnReset);
    }

    SECTION("readExactly")
    {
        auto fake = FakeSocket {};
        fake.pushChunk("abc");
        fake.failAfterChunks(NetErrorCode::ConnReset);

        auto source = net::testing::ScriptedEventSource {};
        auto loop = EventLoop { source };
        auto reader = AsyncBufferedReader { &fake };

        auto got = std::string {};
        auto error = std::optional<net::NetError> {};
        loop.blockOn(readOneExactly(&reader, 16, &got, &error));

        REQUIRE(error.has_value());
        REQUIRE(error->code == NetErrorCode::ConnReset);
    }
}
