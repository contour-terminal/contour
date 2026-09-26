// SPDX-License-Identifier: Apache-2.0
//
// A resource settles a parked operation at once and resumes its waiter through the loop: never
// inline, inside `close()` or `cancelRead()` (guarantee G2, `.agent/rules/async-and-net.md`: every
// resumption happens in turn step 2, and a resource never resumes its consumer inline).
//
// The defect this pins was found by contour. `NativeClient::detach` is
// `_writer.close(); _connection->close();`, and the first close resumed the client's parked read
// INSIDE `close()`; that flow ran to its end and destroyed the client, and the second statement
// then called through a destroyed `_connection`. Deterministic, and an ASan report in the loop's
// own guarantees rather than in contour's code.
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

using core::async::Task;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::testing::BackendMatrix;

namespace
{

/// How a parked read ended.
struct ReadOutcome
{
    bool resolved = false;                ///< Whether the read's flow has run past its `co_await`.
    bool hasValue = false;                ///< Whether it answered with a count.
    NetErrorCode code = NetErrorCode::Ok; ///< The code, where it answered with an error.
};

/// Parks one read and records how it ended.
Task<void> readOnce(ISocket* sock, ReadOutcome* out)
{
    auto buffer = std::array<std::byte, 8> {};
    auto const got = co_await sock->read(buffer);
    out->resolved = true;
    out->hasValue = got.has_value();
    if (!got.has_value())
        out->code = got.error().code;
}

/// Which verb retires the read.
enum class Retire : std::uint8_t
{
    Close,
    CancelRead,
};

/// Retires the parked read and records whether its waiter had already run when the verb returned.
Task<void> retireAndLook(
    ISocket* sock, Retire verb, ReadOutcome const* reader, bool* ranInside, bool* returned)
{
    if (verb == Retire::Close)
        sock->close();
    else
        sock->cancelRead();
    *ranInside = reader->resolved;
    *returned = true;
    co_return;
}

/// Runs turns until @p done or @p bound turns have run; each turn waits at most 10ms.
/// @return How many turns ran.
std::size_t turnUntil(EventLoop& loop, bool const& done, std::size_t bound)
{
    auto turns = std::size_t { 0 };
    while (!done && turns < bound)
    {
        std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
        ++turns;
    }
    return turns;
}

/// Runs turns until something is parked on the loop, at most @p bound of them.
/// @return Whether something parked.
bool turnUntilParked(EventLoop& loop, std::size_t bound)
{
    auto turns = std::size_t { 0 };
    while (turns < bound && loop.parkedWaiterCount() == 0)
    {
        std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
        ++turns;
    }
    return loop.parkedWaiterCount() > 0;
}

/// What contour's client was: an object owning two sockets, destroyed by the flow reading one.
struct Client
{
    std::unique_ptr<ISocket> reader;
    std::unique_ptr<ISocket> writer;
    bool* destroyed;

    Client(std::unique_ptr<ISocket> readSide, std::unique_ptr<ISocket> writeSide, bool* flag) noexcept:
        reader(std::move(readSide)), writer(std::move(writeSide)), destroyed(flag)
    {
    }
    Client(Client const&) = delete;
    Client& operator=(Client const&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;
    ~Client() { *destroyed = true; }
};

/// contour's `runClient`: reads until the read fails, then destroys the client that owns it.
Task<void> runClient(std::unique_ptr<Client>* client, ReadOutcome* out)
{
    co_await readOnce((*client)->reader.get(), out);
    client->reset();
}

/// contour's `NativeClient::detach`: close one socket, then the other, through the client. Records
/// whether the client was already gone between the two -- which, with an inline resume, it was, and
/// the second close would have called through freed storage.
Task<void> detach(std::unique_ptr<Client>* client, bool const* destroyed, bool* goneBetween)
{
    auto* const owner = client->get();
    owner->reader->close();
    *goneBetween = *destroyed;
    if (!*goneBetween)
        owner->writer->close();
    co_return;
}

/// How a read ended, when it may end by unwinding rather than with an answer.
struct EndOutcome
{
    ReadOutcome read;     ///< Filled in when the read answered.
    bool unwound = false; ///< Whether it ended in `OperationCancelled` instead.
    bool ended = false;   ///< Whether the flow ran to its end either way.
};

/// Parks one read, and ends whether the read answers or unwinds.
Task<void> readUntilEnded(ISocket* sock, EndOutcome* out)
{
    try
    {
        co_await readOnce(sock, &out->read);
    }
    catch (core::async::OperationCancelled const&)
    {
        out->unwound = true;
    }
    out->ended = true;
}

/// Retires the parked read and destroys the socket in the same turn: the owner's
/// `conn->close(); connections.erase(id);`.
Task<void> retireAndDestroy(std::unique_ptr<ISocket>* sock, Retire verb)
{
    if (verb == Retire::Close)
        (*sock)->close();
    else
        (*sock)->cancelRead();
    sock->reset();
    co_return;
}

/// A spawned root owning both ends of a pair, parked on a read of the second until teardown
/// destroys it -- and with it the first end, which a borrowed flow is reading.
Task<void> holdUntilTeardown(std::unique_ptr<ISocket> held, std::unique_ptr<ISocket> parkedOn)
{
    auto buffer = std::array<std::byte, 8> {};
    std::ignore = co_await parkedOn->read(buffer);
    std::ignore = held;
}

/// Sets a flag when the frame holding it is destroyed, however that happens.
class MarkOnDestroy
{
  public:
    explicit MarkOnDestroy(bool* flag) noexcept: _flag(flag) {}
    MarkOnDestroy(MarkOnDestroy const&) = delete;
    MarkOnDestroy& operator=(MarkOnDestroy const&) = delete;
    MarkOnDestroy(MarkOnDestroy&&) = delete;
    MarkOnDestroy& operator=(MarkOnDestroy&&) = delete;
    ~MarkOnDestroy() { *_flag = true; }

  private:
    bool* _flag;
};

/// A chain nobody owns -- the loop's to free -- parked on one read.
core::async::DetachedTask readDetached(ISocket* sock, bool* resumed, bool* destroyed)
{
    auto const mark = MarkOnDestroy { destroyed };
    auto buffer = std::array<std::byte, 8> {};
    std::ignore = co_await sock->read(buffer);
    *resumed = true;
}

/// How an accept ended.
struct AcceptOutcome
{
    std::optional<core::net::AcceptResult> result; ///< What it answered, where it answered.
    bool unwound = false;                          ///< Whether it ended in `OperationCancelled`.
    bool ended = false;                            ///< Whether the flow ran to its end either way.
};

/// Parks one accept, and ends whether the accept answers or unwinds.
Task<void> acceptUntilEnded(core::net::IListener* listener, AcceptOutcome* out)
{
    try
    {
        out->result = co_await listener->accept();
    }
    catch (core::async::OperationCancelled const&)
    {
        out->unwound = true;
    }
    out->ended = true;
}

/// Closes the listener and destroys it in one turn: `listener->close(); listener.reset();`.
Task<void> closeAndDestroy(std::unique_ptr<core::net::IListener>* listener)
{
    (*listener)->close();
    listener->reset();
    co_return;
}

/// Which listener a case makes.
enum class ListenerKind : std::uint8_t
{
    Tcp,
    Unix,
};

[[nodiscard]] char const* nameOf(Retire verb) noexcept
{
    return verb == Retire::Close ? "close" : "cancelRead";
}

} // namespace

TEST_CASE("close() and cancelRead() settle a parked read at once and resume it through the loop",
          "[net][socket][resume]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        for (auto const verb: { Retire::Close, Retire::CancelRead })
        {
            DYNAMIC_SECTION("backend=" << backend.name
                                       << " verb=" << (verb == Retire::Close ? "close" : "cancelRead"))
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());

                auto reader = ReadOutcome {};
                loop.spawn(readOnce(pair->first.get(), &reader));
                REQUIRE(turnUntilParked(loop, 8));
                REQUIRE_FALSE(reader.resolved);

                auto ranInside = false;
                auto returned = false;
                loop.spawn(retireAndLook(pair->first.get(), verb, &reader, &ranInside, &returned));
                auto const turns = turnUntil(loop, reader.resolved, 8);

                INFO("turns: " << turns);
                REQUIRE(returned);
                // The waiter had NOT run when the verb returned: it is the loop's to resume ...
                CHECK_FALSE(ranInside);
                // ... and the loop did: in the turn that ran the verb on the reactors, one turn later
                // where IOCP settles a retired real read when the kernel hands it back.
                CHECK(reader.resolved);
                CHECK(turns <= 2);
                // What it resolved to is unchanged: a Cancelled VALUE, because the flow is alive and
                // asked about a read the resource took away.
                CHECK_FALSE(reader.hasValue);
                CHECK(reader.code == NetErrorCode::Cancelled);
            }
        }
    }
}

TEST_CASE("A flow resumed by close() may destroy the socket's owner without close() touching it",
          "[net][socket][resume]")
{
    // contour's crash, as a case. The reading flow destroys the client that owns both sockets once
    // its read ends; the detaching flow closes the read socket and then the write socket through
    // that same client. Resumed inline, the reader destroyed the client INSIDE the first close.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto readPair = core::net::testing::makeSocketPair(loop);
            auto writePair = core::net::testing::makeSocketPair(loop);
            REQUIRE(readPair.has_value());
            REQUIRE(writePair.has_value());

            auto destroyed = false;
            auto client =
                std::make_unique<Client>(std::move(readPair->first), std::move(writePair->first), &destroyed);
            auto reader = ReadOutcome {};
            loop.spawn(runClient(&client, &reader));
            REQUIRE(turnUntilParked(loop, 8));

            auto goneBetween = false;
            loop.spawn(detach(&client, &destroyed, &goneBetween));
            auto const turns = turnUntil(loop, destroyed, 8);

            INFO("turns: " << turns);
            CHECK_FALSE(goneBetween); // the client outlived both closes
            CHECK(destroyed);         // and then its reader destroyed it, on the loop
            CHECK(reader.resolved);
            CHECK(client == nullptr);
        }
    }
}

TEST_CASE(
    "A socket destroyed between close() or cancelRead() and the next turn is not touched by its resumed flow",
    "[net][socket][resume]")
{
    // The owner's `conn->close(); connections.erase(id);` in one turn. The waiter is queued by the
    // verb and resumed a turn later, after the socket is gone, so nothing on its way back may read
    // the socket: a frame-free transport settled a value that does not refer to it, and a
    // coroutine-shaped one must see that the socket is gone and unwind. Before the fix, the
    // removed WFMO transport's `parkUntilReady` resumed on its normal path and wrote into the freed
    // socket -- silent here, a heap-use-after-free under AddressSanitizer.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        for (auto const verb: { Retire::Close, Retire::CancelRead })
        {
            DYNAMIC_SECTION("backend=" << backend.name << " verb=" << nameOf(verb))
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());

                auto outcome = EndOutcome {};
                loop.spawn(readUntilEnded(pair->first.get(), &outcome));
                REQUIRE(turnUntilParked(loop, 8));

                loop.spawn(retireAndDestroy(&pair->first, verb));
                auto const turns = turnUntil(loop, outcome.ended, 8);

                INFO("turns: " << turns << " unwound: " << outcome.unwound);
                REQUIRE(outcome.ended); // or it waited 8 turns for a resume that never came
                CHECK(pair->first == nullptr);
                // Either answer is honest -- a Cancelled VALUE settled before the socket went, or
                // an unwind because it went -- and neither is a byte count.
                CHECK_FALSE(outcome.read.hasValue);
                if (!outcome.unwound)
                    CHECK(outcome.read.code == NetErrorCode::Cancelled);
            }
        }
    }
}

TEST_CASE(
    "A frame destroyed while close() or cancelRead() has its waiter queued is taken out of the ready queue",
    "[net][socket][resume]")
{
    // `ResultAwaitable`'s destructor branch: settled, handed to the loop, and the awaiting frame
    // destroyed before the loop reached it. The ready queue must lose the handle with the frame,
    // or the next drain resumes freed storage.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        for (auto const verb: { Retire::Close, Retire::CancelRead })
        {
            DYNAMIC_SECTION("backend=" << backend.name << " verb=" << nameOf(verb))
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());

                // BORROWED: the case holds the frame, so the case may destroy it.
                auto reader = ReadOutcome {};
                auto flow = readOnce(pair->first.get(), &reader);
                flow.handle().resume();
                REQUIRE_FALSE(flow.handle().done());

                auto const before = loop.readyCount();
                if (verb == Retire::Close)
                    pair->first->close();
                else
                    pair->first->cancelRead();
                auto const queued = loop.readyCount() - before;
                INFO("queued by the verb: " << queued);
                // The verb settles and queues at once -- except IOCP's `cancelRead`, which asks the
                // kernel for the read back and settles when the abort is dequeued, in a drain; that
                // shape has no queued window and its frame, destroyed here, is retired instead.
                auto const settlesAtTheVerb = verb == Retire::Close || backend.name != "iocp";
                REQUIRE(queued == (settlesAtTheVerb ? 1U : 0U));

                flow = {};
                CHECK(loop.readyCount() == before); // taken out with the frame

                for ([[maybe_unused]] auto const turn: { 0, 1, 2 })
                    std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
                CHECK_FALSE(reader.resolved); // nothing resumed the destroyed frame
            }
        }
    }
}

TEST_CASE("Teardown resumes a borrowed flow whose socket destroying a spawned root abandoned",
          "[net][socket][resume][teardown]")
{
    // `~EventLoop` step 5 destroys the spawned roots; a root owning a socket abandons the
    // operation a BORROWED flow has parked on it, which settles and queues that flow. Nothing
    // drained after step 5, so the flow stayed suspended with its operation still naming the loop,
    // and destroying it after the loop called into freed storage. The fix drains what step 5
    // queues: the flow unwinds (an abandoned operation throws) before the loop is gone.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            // Declared before the loop, so it outlives it, as a fixture's member would.
            auto outcome = EndOutcome {};
            auto flow = Task<void> {};
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());

                flow = readUntilEnded(pair->first.get(), &outcome);
                flow.handle().resume();
                REQUIRE_FALSE(flow.handle().done());

                loop.spawn(holdUntilTeardown(std::move(pair->first), std::move(pair->second)));
                for ([[maybe_unused]] auto const turn: { 0, 1, 2 })
                    std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
                REQUIRE_FALSE(outcome.ended);
            }
            auto const done = flow.handle().done();
            CHECK(done);
            CHECK(outcome.ended);
            CHECK(outcome.unwound);
            // Leaked rather than destroyed where the teardown left it suspended: its destructor
            // would call into the destroyed loop, the defect the CHECKs above report.
            if (!done)
                std::ignore = flow.release();
        }
    }
}

TEST_CASE("A detached flow close() queued is freed by teardown, not resumed",
          "[net][socket][resume][teardown]")
{
    // A socket settles the operation and hands its waiter to the loop. The loop has to know whether
    // that chain is its own: at teardown it FREES what it owns -- a `DetachedTask`, which has no
    // stop token, so resuming it would run the rest of its body on a dying loop -- and resumes what
    // it borrows. Handed over as a bare handle, every such chain was filed as borrowed and resumed.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto resumed = false;
            auto destroyed = false;
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());

                readDetached(pair->first.get(), &resumed, &destroyed);
                REQUIRE(turnUntilParked(loop, 8));
                REQUIRE_FALSE(destroyed);

                pair->first->close(); // settled and queued; no turn runs it
            }
            CHECK(destroyed);     // freed with the loop, as the loop's own chain
            CHECK_FALSE(resumed); // and never run past its `co_await`
        }
    }
}

TEST_CASE("A listener closed and destroyed in one turn is not touched by its resumed accept",
          "[net][listener][resume]")
{
    // `close()` wakes the parked accept through the loop, a turn later; the owner destroys the
    // listener before that turn. The accept then resumed on its NORMAL path and read the listener's
    // `_closed` and descriptor from freed storage -- `PosixListener`, `UnixListener` and WFMO's
    // `WindowsListener` alike; an IOCP accept answers from state it shares. It must see the
    // listener is gone and answer without it.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        for (auto const kind: { ListenerKind::Tcp, ListenerKind::Unix })
        {
            DYNAMIC_SECTION("backend=" << backend.name
                                       << " listener=" << (kind == ListenerKind::Tcp ? "tcp" : "unix"))
            {
                auto loop = EventLoop { *source };
                auto const directory = std::filesystem::temp_directory_path()
                                       / std::format("core-cpp-accept-{}", std::random_device {}());
                auto bound = kind == ListenerKind::Tcp
                                 ? core::net::listen(loop, "127.0.0.1", 0)
                                 : core::net::listenUnix(loop, (directory / "accept.sock").string());
                REQUIRE(bound.has_value());
                auto listener = std::move(*bound);

                auto outcome = AcceptOutcome {};
                loop.spawn(acceptUntilEnded(listener.get(), &outcome));
                REQUIRE(turnUntilParked(loop, 8));

                loop.spawn(closeAndDestroy(&listener));
                auto const turns = turnUntil(loop, outcome.ended, 8);
                auto ec = std::error_code {};
                std::filesystem::remove_all(directory, ec);

                INFO("turns: " << turns << " unwound: " << outcome.unwound);
                REQUIRE(outcome.ended);
                CHECK(listener == nullptr);
                // `IListener::close` answers a parked accept with a Cancelled VALUE.
                REQUIRE_FALSE(outcome.unwound);
                REQUIRE(outcome.result.has_value());
                REQUIRE_FALSE(outcome.result->has_value());
                CHECK(outcome.result->error().code == NetErrorCode::Cancelled);
                // And it said so from the lifetime token, not from the freed `_closed` -- which is
                // what tells the fix apart where no sanitizer is watching.
                if (backend.name != "iocp")
                    CHECK(outcome.result->error().context == "the listener was destroyed");
            }
        }
    }
}
