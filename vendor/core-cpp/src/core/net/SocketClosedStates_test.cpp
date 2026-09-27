// SPDX-License-Identifier: Apache-2.0
//
// What `testing::InMemorySocket` answers in every closed state, pinned against a real loopback pair
// driven through the same steps.
//
// **The fake is the transport a consumer builds its protocol, server and consensus suites on**, so a
// state it answers more permissively than a real socket is a state no test anywhere can see: a write
// to a peer that had closed used to succeed, and so no in-memory case could notice a session the
// other end had ended. That does not fail -- it manufactures passing tests downstream, which is worse
// than a missing test because it is invisible and it compounds.
//
// Each sequence below is a table of steps, each naming the end that runs it and what the model
// answers. The case runs the table twice -- over `InMemorySocketPair`, and over a real accepted
// socket facing a real dialled one on this platform's loop -- and asserts that the fake answered the
// table AND that the real pair answered it too. **That is the property, and a case that ran only one
// of the two would not be this test**: the fake alone proves the fake agrees with a table somebody
// wrote, and the real pair alone proves nothing about the fake.
//
// **Where the platforms disagree, the model takes the stricter answer, and that is Windows's.**
// Measured on loopback upstream (Windows 11, Windows Server 2025, Linux under WSL2, macOS 14;
// fastcached#1553):
//   - after a reset, Linux and macOS hand over bytes that were already buffered and then report EOF
//     again; Windows fails every read, buffered bytes included;
//   - a write after a FIN or a reset fails on every platform, as `EPIPE` on POSIX and as
//     `WSAECONNABORTED` or `WSAECONNRESET` on Windows -- and POSIX reports a pending reset to
//     whichever call comes first, so the code a later call gets depends on the order.
// Every failing row names the code the platform error is mapped to. A row marked `CodeMayDiffer` lets
// Linux and macOS fail under another code, and one marked `PlatformMayAnswer` lets them answer where
// the model fails. **On Windows neither is granted**: the model IS Windows, error codes included, and
// a Windows run that differs means the justification above has stopped being true.
//
// Origin: fastcached `src/FastCache/Net/SocketClosedStates_test.cpp`
// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`). Upstream's real peer was a blocking socket on a
// thread of its own; here both real ends are reactor sockets on one loop, run one step at a time,
// which asks the kernel the same questions without a second thread to coordinate.
#include <core/async/DetachedTask.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/NetError.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemorySocket.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using core::async::Task;
using core::net::EventLoop;
using core::net::IListener;
using core::net::IoResult;
using core::net::ISocket;
using core::net::NetErrorCode;

/// Which socket of the pair a step runs on.
enum class End : std::uint8_t
{
    Observed, ///< The accepted socket: the kind the fake stands in for.
    Peer,     ///< The dialling socket.
};

/// What a step does.
enum class Act : std::uint8_t
{
    Write,
    WriteVectored,
    Read,
    WaitReadable,
    ShutdownWrite,
    Close,
};

/// How an operation answered, in terms two transports can be compared in.
enum class Answer : std::uint8_t
{
    NotRun, ///< The run ended before this step: a step outlived its bound, or a dial failed.
    Done,   ///< `close` or `shutdownWrite`, which answer nothing.
    Bytes,  ///< A read or a write moved bytes.
    Ready,  ///< `waitReadable` reported data.
    Eof,    ///< A read or `waitReadable` reported the end of the stream.
    Failed, ///< Any other error: a reset, `EPIPE`, `WSAESHUTDOWN`.
    Closed, ///< `BadHandle`: this end had closed.
    Retry,  ///< `WouldBlock`, the one failure a caller may retry.
};

/// Where a platform may answer differently from the model. On Windows every row is `Exact`,
/// whatever it says: the model is Windows.
enum class Latitude : std::uint8_t
{
    Exact,             ///< Every platform answers what the model does, error code included.
    CodeMayDiffer,     ///< Every platform fails; Linux and macOS may name the failure another way.
    PlatformMayAnswer, ///< The model fails; Linux and macOS may answer, or fail another way.
};

/// One row per enumerator, in enumerator order; @c rowOf asserts the order rather than trusting it.
template <typename Enum, typename Row, std::size_t N>
[[nodiscard]] constexpr Row const& rowOf(std::array<Row, N> const& rows, Enum value)
{
    auto const& row = rows[static_cast<std::size_t>(value)];
    // A table whose rows drifted out of order would answer for the wrong enumerator, silently. In a
    // constant expression a failed assert does not compile, which is what the static_asserts below
    // rely on.
    assert(row.key == value && "table row out of enumerator order");
    return row;
}

struct EndRow
{
    End key;
    std::string_view name;
};

constexpr auto EndRows = std::array {
    EndRow { .key = End::Observed, .name = "observed" },
    EndRow { .key = End::Peer, .name = "peer" },
};

struct ActRow
{
    Act key;
    std::string_view name;
    bool sends; ///< Puts something on the wire, so the real run lets it land before the next step.
};

constexpr auto ActRows = std::array {
    ActRow { .key = Act::Write, .name = "write", .sends = true },
    ActRow { .key = Act::WriteVectored, .name = "write-vectored", .sends = true },
    ActRow { .key = Act::Read, .name = "read", .sends = false },
    ActRow { .key = Act::WaitReadable, .name = "wait-readable", .sends = false },
    ActRow { .key = Act::ShutdownWrite, .name = "shutdown-write", .sends = true },
    ActRow { .key = Act::Close, .name = "close", .sends = true },
};

struct AnswerRow
{
    Answer key;
    std::string_view name;
    bool answered; ///< An answer a platform may give where the model fails, on a `PlatformMayAnswer` row.
    bool failure;  ///< An error, so its code is part of the answer.
};

constexpr auto AnswerRows = std::array {
    AnswerRow { .key = Answer::NotRun, .name = "not-run", .answered = false, .failure = false },
    AnswerRow { .key = Answer::Done, .name = "done", .answered = false, .failure = false },
    AnswerRow { .key = Answer::Bytes, .name = "bytes", .answered = true, .failure = false },
    AnswerRow { .key = Answer::Ready, .name = "ready", .answered = true, .failure = false },
    AnswerRow { .key = Answer::Eof, .name = "eof", .answered = true, .failure = false },
    AnswerRow { .key = Answer::Failed, .name = "failed", .answered = false, .failure = true },
    AnswerRow { .key = Answer::Closed, .name = "closed", .answered = false, .failure = true },
    AnswerRow { .key = Answer::Retry, .name = "retry", .answered = false, .failure = true },
};

// Every row is checked at compile time, so the tables cannot drift from their enumerations.
static_assert(std::ranges::all_of(EndRows, [](auto const& row) { return &rowOf(EndRows, row.key) == &row; }));
static_assert(std::ranges::all_of(ActRows, [](auto const& row) { return &rowOf(ActRows, row.key) == &row; }));
static_assert(std::ranges::all_of(AnswerRows,
                                  [](auto const& row) { return &rowOf(AnswerRows, row.key) == &row; }));

/// The errors that are answers of their own; every other error is `Failed`.
constexpr auto ErrorAnswers = std::array {
    std::pair { NetErrorCode::BadHandle, Answer::Closed },
    std::pair { NetErrorCode::WouldBlock, Answer::Retry },
};

// The one platform test in this file, and its answer is a fact ABOUT the platform rather than a
// branch in the code under test: whether the model is this platform's own answers, so no row's
// latitude applies. (It once had a second: whether the platform socket implements a receive
// deadline at all. `WindowsSocket` does not, and since Task B7b the platform loop's socket on
// Windows is `IocpSocket`, which does.)
#ifdef _WIN32
constexpr bool ModelIsThisPlatform = true;
#else
constexpr bool ModelIsThisPlatform = false;
#endif

/// How long the real run lets whatever a step sent -- bytes, a FIN, a reset -- land before the
/// next step. Loopback delivers well inside it on every platform measured; the margin is for a
/// loaded runner.
constexpr auto Settle = 50ms;

/// How long one real operation may take. Every step is chosen so it answers at once, so this
/// bounds only a platform that parks where the table expects an answer -- and says so, rather than
/// hanging until ctest's timeout names nothing.
constexpr auto OperationBound = 5000ms;

/// Big enough for every read a table expects.
constexpr std::size_t ReadBufferBytes = 16;

// The codes platform errors are mapped to, by what they mean here.
/// The peer closed over bytes it had not read: `ECONNRESET`, `WSAECONNRESET`.
constexpr auto Reset = NetErrorCode::ConnReset;
/// A write after the peer's FIN drew the reset, or this end half-closed and wrote: `EPIPE`,
/// `WSAECONNABORTED`, `WSAESHUTDOWN`.
constexpr auto Refused = NetErrorCode::SystemError;
/// This end closed.
constexpr auto Gone = NetErrorCode::BadHandle;
/// No error: the step answers. A placeholder only; a row's `model` says whether it failed.
constexpr auto None = NetErrorCode::SystemError;

/// One operation of a sequence.
struct Step
{
    End end;                               ///< Which end runs it.
    Act act;                               ///< What it does.
    std::size_t bytes;                     ///< What a write sends, and what a `Bytes` answer moves.
    Answer model;                          ///< What `InMemorySocket` answers.
    NetErrorCode code { None };            ///< The error it answers with, when it fails.
    Latitude latitude { Latitude::Exact }; ///< Where a platform may answer differently.
};

/// A named table of steps, run from the first to the last.
struct Sequence
{
    std::string_view name;
    std::span<Step const> steps;
};

/// A step that moves @p bytes, or answers with nothing to count.
constexpr Step moves(End end, Act act, std::size_t bytes, Answer model = Answer::Bytes)
{
    return Step {
        .end = end, .act = act, .bytes = bytes, .model = model, .code = None, .latitude = Latitude::Exact
    };
}

/// A step the model fails with @p code, and how far a platform may stray from it.
constexpr Step fails(End end, Act act, NetErrorCode code, Latitude latitude = Latitude::Exact)
{
    auto const answer = code == Gone ? Answer::Closed : Answer::Failed;
    return Step { .end = end, .act = act, .bytes = 1, .model = answer, .code = code, .latitude = latitude };
}

constexpr auto Observed = End::Observed;
constexpr auto Peer = End::Peer;

// The peer closes, having read everything it was sent: a FIN. The FIRST write after it is accepted
// and lost, and draws the reset that fails everything after.
constexpr auto PeerClosesGracefully = std::array {
    moves(Peer, Act::Write, 3),
    moves(Observed, Act::Read, 3),
    moves(Peer, Act::Close, 0, Answer::Done),
    moves(Observed, Act::Read, 0, Answer::Eof),
    moves(Observed, Act::WaitReadable, 0, Answer::Eof),
    moves(Observed, Act::Read, 0, Answer::Eof),
    moves(Observed, Act::Write, 1),
    fails(Observed, Act::Write, Refused),
    fails(Observed, Act::WriteVectored, Refused),
    // macOS reports the reset to the first read after it and EOF to the second; Linux reads EOF
    // throughout. Windows fails both, as it does the writes.
    fails(Observed, Act::Read, Refused, Latitude::PlatformMayAnswer),
    fails(Observed, Act::Read, Refused, Latitude::PlatformMayAnswer),
};

// The peer closes with bytes still buffered for US: a FIN, and our first write draws the reset.
// Linux and macOS still hand those bytes over; Windows drops them.
constexpr auto PeerClosesWithBytesForUs = std::array {
    moves(Peer, Act::Write, 3),
    moves(Peer, Act::Close, 0, Answer::Done),
    moves(Observed, Act::Write, 1),
    fails(Observed, Act::Read, Refused, Latitude::PlatformMayAnswer),
    // Pending as `ECONNRESET` on macOS, where Linux and Windows name it the other way.
    fails(Observed, Act::Write, Refused, Latitude::CodeMayDiffer),
};

// The peer closes with OUR bytes unread: a reset at once, so no write is ever accepted.
constexpr auto PeerResets = std::array {
    moves(Observed, Act::Write, 4),
    moves(Peer, Act::Write, 3),
    moves(Peer, Act::Close, 0, Answer::Done),
    // Linux and macOS hand over the three bytes that arrived before the reset.
    fails(Observed, Act::Read, Reset, Latitude::PlatformMayAnswer),
    // The reset itself, which every platform reports, and names alike.
    fails(Observed, Act::Read, Reset),
    // Reported once on Linux and macOS, EOF after it.
    fails(Observed, Act::Read, Reset, Latitude::PlatformMayAnswer),
    // `EPIPE` on Linux and macOS once a read has reported the reset; Windows repeats it.
    fails(Observed, Act::Write, Reset, Latitude::CodeMayDiffer),
    fails(Observed, Act::Write, Reset, Latitude::CodeMayDiffer),
};

// The peer HALF-closes: it has finished sending, not gone, so it still reads -- and its own writes
// fail.
constexpr auto PeerHalfCloses = std::array {
    moves(Peer, Act::Write, 3),
    moves(Peer, Act::ShutdownWrite, 0, Answer::Done),
    moves(Observed, Act::Read, 3),
    moves(Observed, Act::Read, 0, Answer::Eof),
    moves(Observed, Act::WaitReadable, 0, Answer::Eof),
    moves(Observed, Act::Write, 2),
    moves(Peer, Act::Read, 2),
    moves(Observed, Act::WriteVectored, 2),
    moves(Peer, Act::Read, 2),
    fails(Peer, Act::Write, Refused),
};

// THIS end half-closes: its writes fail, and it still reads.
constexpr auto ThisEndHalfCloses = std::array {
    moves(Observed, Act::ShutdownWrite, 0, Answer::Done),
    fails(Observed, Act::Write, Refused),
    fails(Observed, Act::WriteVectored, Refused),
    moves(Peer, Act::Read, 0, Answer::Eof),
    moves(Peer, Act::Write, 3),
    moves(Observed, Act::Read, 3),
    moves(Peer, Act::Close, 0, Answer::Done),
    moves(Observed, Act::Read, 0, Answer::Eof),
};

// THIS end closes, having read everything: every operation on it is refused as a closed handle,
// `shutdownWrite` does nothing, and the peer meets the FIN and then the reset.
constexpr auto ThisEndCloses = std::array {
    moves(Observed, Act::Close, 0, Answer::Done),
    fails(Observed, Act::Read, Gone),
    fails(Observed, Act::Write, Gone),
    fails(Observed, Act::WriteVectored, Gone),
    fails(Observed, Act::WaitReadable, Gone),
    // Succeeds and does nothing on every platform socket after a close.
    moves(Observed, Act::ShutdownWrite, 0, Answer::Done),
    moves(Peer, Act::Read, 0, Answer::Eof),
    moves(Peer, Act::Write, 1),
    fails(Peer, Act::Write, Refused),
    fails(Peer, Act::Read, Refused, Latitude::PlatformMayAnswer),
};

// THIS end closes with the peer's bytes unread: a reset at once. This is the shape a server that
// answers and then closes on a request it did not finish reading has, and why a refusal written
// just before such a close can be lost.
constexpr auto ThisEndResets = std::array {
    moves(Peer, Act::Write, 3),
    moves(Observed, Act::Close, 0, Answer::Done),
    fails(Peer, Act::Read, Reset),
    fails(Peer, Act::Write, Reset, Latitude::CodeMayDiffer),
};

constexpr auto Sequences = std::array {
    Sequence { .name = "the peer closes, having read everything", .steps = PeerClosesGracefully },
    Sequence { .name = "the peer closes with bytes buffered for us", .steps = PeerClosesWithBytesForUs },
    Sequence { .name = "the peer closes with our bytes unread", .steps = PeerResets },
    Sequence { .name = "the peer half-closes", .steps = PeerHalfCloses },
    Sequence { .name = "this end half-closes", .steps = ThisEndHalfCloses },
    Sequence { .name = "this end closes, having read everything", .steps = ThisEndCloses },
    Sequence { .name = "this end closes with the peer's bytes unread", .steps = ThisEndResets },
};

/// One step's answer, as either run recorded it.
struct Recorded
{
    Answer answer { Answer::NotRun }; ///< How it answered.
    std::size_t bytes { 0 };          ///< What it moved, when it moved bytes.
    NetErrorCode code { None };       ///< The error it answered with, when it failed.
    std::string detail;               ///< The error in words, or why the step did not run.
};

/// Classifies one operation's result.
/// @param result What the operation completed with.
/// @param act What the operation was, since `waitReadable` reports readiness rather than bytes.
/// @return The answer.
[[nodiscard]] Recorded classify(IoResult const& result, Act act)
{
    if (!result.has_value())
    {
        auto const& error = result.error();
        auto answer = Answer::Failed;
        for (auto const& [code, answered]: ErrorAnswers)
            if (code == error.code)
                answer = answered;
        return Recorded { .answer = answer, .bytes = 0, .code = error.code, .detail = error.toString() };
    }
    if (*result == 0)
        return Recorded { .answer = Answer::Eof, .bytes = 0, .code = None, .detail = {} };
    return Recorded { .answer = act == Act::WaitReadable ? Answer::Ready : Answer::Bytes,
                      .bytes = *result,
                      .code = None,
                      .detail = {} };
}

/// A step that answers nothing.
[[nodiscard]] Recorded done()
{
    return Recorded { .answer = Answer::Done, .bytes = 0, .code = None, .detail = {} };
}

/// Runs one step on @p socket and classifies its answer. The ONE place either run touches a
/// socket, so the two cannot drift into asking different questions.
/// @param socket The socket the step names.
/// @param step The step.
/// @return Its answer.
Task<Recorded> perform(ISocket* socket, Step step)
{
    auto const payload = std::vector<std::byte>(step.bytes, std::byte { 0x5a });
    auto const bytes = std::span<std::byte const> { payload };
    auto buffer = std::array<std::byte, ReadBufferBytes> {};
    switch (step.act)
    {
        case Act::Write: co_return classify(co_await socket->write(bytes), step.act);
        case Act::WriteVectored: {
            auto const split = std::min<std::size_t>(1, bytes.size());
            auto const segments = std::array { bytes.first(split), bytes.subspan(split) };
            co_return classify(co_await socket->writeVectored(segments), step.act);
        }
        case Act::Read: co_return classify(co_await socket->read(buffer), step.act);
        case Act::WaitReadable: co_return classify(co_await socket->waitReadable(), step.act);
        case Act::ShutdownWrite: {
            // A failed half-close would be a step answering something the table has no row for;
            // recorded as `Failed` so the transcript says so rather than reading as `done`.
            auto const shut = co_await socket->shutdownWrite();
            if (!shut.has_value())
                co_return Recorded { .answer = Answer::Failed,
                                     .bytes = 0,
                                     .code = shut.error().code,
                                     .detail = shut.error().toString() };
            co_return done();
        }
        case Act::Close: socket->close(); co_return done();
    }
    co_return Recorded { .answer = Answer::NotRun, .bytes = 0, .code = None, .detail = "no such act" };
}

/// @return Whether @p recorded is exactly what @p step says the model answers, error code included.
[[nodiscard]] bool matches(Recorded const& recorded, Step const& step) noexcept
{
    auto const& row = rowOf(AnswerRows, recorded.answer);
    return recorded.answer == step.model && (recorded.answer != Answer::Bytes || recorded.bytes == step.bytes)
           && (!row.failure || recorded.code == step.code);
}

/// @return Whether a real socket's @p recorded answer is one the model allows for @p step.
[[nodiscard]] bool admits(Recorded const& recorded, Step const& step) noexcept
{
    if (matches(recorded, step))
        return true;
    if (ModelIsThisPlatform)
        return false;
    // The ways a platform may differ, and only on a row that says so. Never by answering where the
    // model fails on a row that does not grant it, which would make the fake the more permissive of
    // the two.
    auto const failedAlike = recorded.answer == step.model && rowOf(AnswerRows, step.model).failure;
    switch (step.latitude)
    {
        case Latitude::Exact: return false;
        case Latitude::CodeMayDiffer: return failedAlike;
        case Latitude::PlatformMayAnswer: return failedAlike || rowOf(AnswerRows, recorded.answer).answered;
    }
    return false;
}

/// @return @p recorded in words, for the transcript.
[[nodiscard]] std::string describe(Recorded const& recorded)
{
    auto text = std::string { rowOf(AnswerRows, recorded.answer).name };
    if (recorded.answer == Answer::Bytes || recorded.answer == Answer::Ready)
        text += "(" + std::to_string(recorded.bytes) + ")";
    if (!recorded.detail.empty())
        text += " " + recorded.detail;
    return text;
}

/// @return @p text left-aligned in @p width columns.
[[nodiscard]] std::string padded(std::string text, std::size_t width)
{
    text.resize(std::max(text.size(), width), ' ');
    return text;
}

/// Both runs side by side, one line per step.
[[nodiscard]] std::string transcript(std::span<Step const> steps,
                                     std::span<Recorded const> fake,
                                     std::span<Recorded const> real)
{
    auto text = std::string { "step end       act             model                 fake / real" };
    for (auto const index: std::views::iota(std::size_t { 0 }, steps.size()))
    {
        auto const& step = steps[index];
        auto model = std::string { rowOf(AnswerRows, step.model).name };
        if (step.model == Answer::Bytes)
            model += "(" + std::to_string(step.bytes) + ")";
        if (rowOf(AnswerRows, step.model).failure)
            model += " " + std::string { core::net::toString(step.code) };
        text += "\n" + padded(std::to_string(index), 5)
                + padded(std::string { rowOf(EndRows, step.end).name }, 10)
                + padded(std::string { rowOf(ActRows, step.act).name }, 16) + padded(model, 22)
                + describe(fake[index]) + "  /  " + describe(real[index]);
    }
    return text;
}

/// Runs @p steps over an `InMemorySocketPair`: no loop, no descriptor, every answer inline.
[[nodiscard]] std::vector<Recorded> runInMemory(std::span<Step const> steps)
{
    auto pair = core::net::testing::InMemorySocketPair::create();
    auto answers = std::vector<Recorded> {};
    for (auto const& step: steps)
    {
        auto* const socket = step.end == End::Observed ? pair.server.get() : pair.client.get();
        answers.push_back(core::async::syncRun(perform(socket, step)));
    }
    return answers;
}

/// A real connected pair, the accepted end and the dialled one.
struct RealPair
{
    std::unique_ptr<ISocket> observed; ///< The accepted end.
    std::unique_ptr<ISocket> peer;     ///< The dialled end.
};

/// Dials @p listener and accepts the connection. A loopback dial completes out of the kernel's
/// backlog before anything calls `accept`, so the two awaits in sequence cannot wait on each other.
Task<void> connectPair(EventLoop* loop, IListener* listener, RealPair* out)
{
    auto dialled = co_await core::net::connect(loop, "127.0.0.1", listener->boundPort());
    if (!dialled.has_value())
        co_return;
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    out->peer = std::move(*dialled);
    out->observed = std::move(*accepted);
}

/// Runs one step and publishes its answer, for @c boundedStep's race.
Task<void> performInto(ISocket* socket, Step step, Recorded* out)
{
    *out = co_await perform(socket, step);
}

/// One real step, bounded: the step against a timer, and whichever finishes first wins. A step
/// the table expects to answer at once and that PARKS instead is a platform disagreeing with the
/// model, and it is reported as that -- the timer's win leaves the answer `NotRun` -- rather than
/// as a hang ctest names nothing about.
Task<void> boundedStep(EventLoop* loop, ISocket* socket, Step step, Recorded* out)
{
    co_await core::net::testing::anyOf(performInto(socket, step, out),
                                       core::net::testing::sleepFor(loop, OperationBound));
    if (out->answer != Answer::NotRun && rowOf(ActRows, step.act).sends)
        co_await core::net::testing::sleepFor(loop, Settle);
}

/// Runs @p steps over a real loopback pair on this platform's loop.
/// @param steps The sequence.
/// @param account Set to why the run stopped early, when it did.
/// @return One answer per step; `NotRun` from the first step that did not answer.
[[nodiscard]] std::vector<Recorded> runOnLoopback(std::span<Step const> steps, std::string& account)
{
    auto loop = core::net::PlatformLoop {};
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    REQUIRE((*listener)->boundPort() != 0);

    auto pair = RealPair {};
    loop.blockOn(connectPair(&loop, listener->get(), &pair));
    auto answers = std::vector<Recorded>(steps.size());
    if (pair.observed == nullptr || pair.peer == nullptr)
    {
        account = "the loopback pair could not be connected";
        return answers;
    }

    for (auto const index: std::views::iota(std::size_t { 0 }, steps.size()))
    {
        auto* const socket = steps[index].end == End::Observed ? pair.observed.get() : pair.peer.get();
        loop.blockOn(boundedStep(&loop, socket, steps[index], &answers[index]));
        if (answers[index].answer == Answer::NotRun)
        {
            account = "step " + std::to_string(index) + " outlived its bound of "
                      + std::to_string(OperationBound.count()) + "ms";
            break;
        }
    }
    return answers;
}

} // namespace

TEST_CASE("InMemorySocket answers every closed state the way a loopback TCP socket does",
          "[net][socket][closed-states]")
{
    for (auto const& sequence: Sequences)
    {
        DYNAMIC_SECTION(sequence.name)
        {
            auto const fake = runInMemory(sequence.steps);
            auto account = std::string {};
            auto const real = runOnLoopback(sequence.steps, account);

            INFO(transcript(sequence.steps, fake, real));
            // First, so a run that never finished is read as that and not as a disagreement.
            CHECK(account.empty());
            for (auto const index: std::views::iota(std::size_t { 0 }, sequence.steps.size()))
            {
                INFO("step " << index);
                auto const& step = sequence.steps[index];
                // The model is what the table says...
                CHECK(matches(fake[index], step));
                // ...and a real socket answers it too: never failing where the fake answers, and
                // answering where the fake fails only on a row that grants it.
                CHECK(admits(real[index], step));
            }
        }
    }
}

namespace
{

/// What a read started after the deadline calls answered, published from an eager coroutine so a
/// case can look at it while it is still parked.
struct ParkedRead
{
    bool resolved { false };
    std::optional<IoResult> result;
};

/// Reads once from @p socket and publishes the answer.
core::async::DetachedTask readOnce(ISocket* socket, ParkedRead* out)
{
    auto buffer = std::array<std::byte, ReadBufferBytes> {};
    out->result = co_await socket->read(buffer);
    out->resolved = true;
}

/// Writes @p count bytes from @p socket.
Task<void> writeBytes(ISocket* socket, std::size_t count)
{
    auto const payload = std::vector<std::byte>(count, std::byte { 0x5a });
    std::ignore = co_await socket->write(payload);
}

} // namespace

TEST_CASE("A receive deadline of zero removes the bound on a real socket", "[net][socket][deadline]")
{
    // `ISocket::setReceiveDeadline`: a positive duration bounds reads started after it, and a
    // NON-POSITIVE one removes the bound. That second half changed meaning -- zero once meant "leave
    // the current setting alone" -- so it is asserted here: a read started after the zero waits for
    // data rather than expiring.
    //
    // The real socket only. The fake has no receive deadline at all (`InMemorySocket.hpp` says so),
    // so a section over it would pass whatever zero meant, and a section that cannot fail asserts
    // nothing.
    constexpr auto Bound = 50ms;
    constexpr auto Outlast = 300ms;

    SECTION("a real socket: the read outlives the old bound, then answers the peer's bytes")
    {
        auto loop = core::net::PlatformLoop {};
        auto listener = core::net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        auto pair = RealPair {};
        loop.blockOn(connectPair(&loop, listener->get(), &pair));
        REQUIRE(pair.observed != nullptr);

        pair.observed->setReceiveDeadline(Bound);
        pair.observed->setReceiveDeadline(0ms);

        auto read = ParkedRead {};
        readOnce(pair.observed.get(), &read);
        loop.blockOn(core::net::testing::sleepFor(&loop, Outlast));
        REQUIRE_FALSE(read.resolved); // six times the old bound, and nothing expired

        loop.blockOn(writeBytes(pair.peer.get(), 3));
        loop.blockOn(core::net::testing::waitUntil(&loop, [&read] { return read.resolved; }));
        REQUIRE(read.resolved);
        REQUIRE(read.result.has_value());
        REQUIRE(read.result->has_value());
        CHECK(**read.result == 3);
    }

    SECTION("a real socket, the control: without the zero the same read expires")
    {
        // Without this, "the zero removed the bound" and "the bound never worked" are one passing
        // section.
        auto loop = core::net::PlatformLoop {};
        auto listener = core::net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        auto pair = RealPair {};
        loop.blockOn(connectPair(&loop, listener->get(), &pair));
        REQUIRE(pair.observed != nullptr);

        pair.observed->setReceiveDeadline(Bound);

        auto read = ParkedRead {};
        readOnce(pair.observed.get(), &read);
        loop.blockOn(core::net::testing::waitUntil(&loop, [&read] { return read.resolved; }));
        REQUIRE(read.resolved);
        REQUIRE(read.result.has_value());
        REQUIRE_FALSE(read.result->has_value());
        CHECK(core::net::isDeadlineExpiry(read.result->error().code));
    }
}
