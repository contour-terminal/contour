// SPDX-License-Identifier: Apache-2.0
//
// The byte channel between Contour and its extension host process: both ways, closing, and what a child
// process does not inherit.
#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <future>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/testing/ClosingReceive.hpp>
#include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>
#include <vtextension/wasm/testing/Wine.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/socket.h>

    #include <csignal>

    #include <fcntl.h>
    #include <unistd.h>

    #include <vtextension/wasm/remote/proto/Codec.hpp>
    #include <vtextension/wasm/remote/proto/Pdu.hpp>
    #include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
    #include <vtextension/wasm/remote/transport/PduStream.hpp>
#endif

using vtextension::wasm::remote::transport::Channel;
using vtextension::wasm::remote::transport::ChannelError;
using vtextension::wasm::remote::transport::ChannelPair;
using vtextension::wasm::remote::transport::makeChannelPair;
using vtextension::wasm::remote::transport::UpwardDescriptors;
using vtextension::wasm::remote::transport::testing::closeHandle;
using vtextension::wasm::remote::transport::testing::duplicate;

namespace
{

/// @return Exactly @p count bytes received off @p channel; nothing if it failed first.
[[nodiscard]] std::optional<std::vector<uint8_t>> receiveExactly(Channel& channel, std::size_t count)
{
    auto received = std::vector<uint8_t>(count);
    auto filled = std::size_t { 0 };
    while (filled < count)
    {
        auto const some = channel.receiveSome(std::span { received }.subspan(filled));
        if (!some)
            return std::nullopt;
        filled += *some;
    }
    return received;
}

} // namespace

TEST_CASE("Channel.pairCarriesBytesBothWays", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());

    auto const request = std::vector<uint8_t> { 1, 2, 3, 4, 5 };
    REQUIRE(pair->parent.send(request).has_value());
    CHECK(receiveExactly(pair->helper, request.size()) == request);

    // Larger than any socket buffer: the sender blocks until the receiver drains it.
    auto const reply = std::vector<uint8_t>(std::size_t { 4 } * 1024 * 1024, uint8_t { 0x5a });
    auto sent = std::async(std::launch::async, [&] { return pair->helper.send(reply); });
    CHECK(receiveExactly(pair->parent, reply.size()) == reply);
    CHECK(sent.get().has_value());
}

TEST_CASE("Channel.shutdownUnblocksReceive", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());

    auto result = std::promise<std::expected<std::size_t, ChannelError>> {};
    auto received = result.get_future();
    auto reader = std::jthread { [&] {
        auto buffer = std::array<uint8_t, 16> {};
        result.set_value(pair->parent.receiveSome(buffer));
    } };
    // Whether the reader blocked before the shutdown or arrives after it, it returns Closed.
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    pair->parent.shutdown();
    CHECK(received.get() == std::unexpected(ChannelError::Closed));

    // And so does every later receive and send on that end.
    auto buffer = std::array<uint8_t, 16> {};
    CHECK(pair->parent.receiveSome(buffer) == std::unexpected(ChannelError::Closed));
    CHECK(pair->parent.send(std::vector<uint8_t> { 1 }) == std::unexpected(ChannelError::Closed));
}

TEST_CASE("Channel.shutdownEndsThePeersStream", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());

    auto result = std::promise<std::expected<std::size_t, ChannelError>> {};
    auto received = result.get_future();
    auto peer = std::jthread { [&] {
        auto buffer = std::array<uint8_t, 16> {};
        result.set_value(pair->helper.receiveSome(buffer));
    } };
    // Whether the peer blocked before the shutdown or arrives after it, its stream has ended.
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    pair->parent.shutdown();
    CHECK(received.get() == std::unexpected(ChannelError::Closed));
    CHECK(pair->helper.send(std::vector<uint8_t>(1024, uint8_t { 1 }))
          == std::unexpected(ChannelError::Closed));
}

TEST_CASE("Channel.peerCloseEndsTheStream", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    {
        auto const helper = std::move(pair->helper);
    }
    auto buffer = std::array<uint8_t, 16> {};
    CHECK(pair->parent.receiveSome(buffer) == std::unexpected(ChannelError::Closed));
}

TEST_CASE("Channel.sendToClosedPeerFails", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
#ifndef _WIN32
    struct sigaction before {};
    REQUIRE(::sigaction(SIGPIPE, nullptr, &before) == 0);
#endif
    {
        auto const helper = std::move(pair->helper);
    }
    CHECK(pair->parent.send(std::vector<uint8_t>(1024, uint8_t { 1 }))
          == std::unexpected(ChannelError::Closed));
#ifndef _WIN32
    // The process survived the write without a SIGPIPE, and without anyone having to ignore the signal.
    struct sigaction after {};
    REQUIRE(::sigaction(SIGPIPE, nullptr, &after) == 0);
    CHECK(after.sa_handler == before.sa_handler);
#endif
}

#ifndef _WIN32
TEST_CASE("Channel.endsAreCloseOnExec", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    for (auto const* channel: { &pair->parent, &pair->helper })
    {
        auto const flags = ::fcntl(channel->nativeHandle(), F_GETFD);
        REQUIRE(flags != -1);
        CHECK((flags & FD_CLOEXEC) != 0);
    }
}
#endif

#ifdef _WIN32
TEST_CASE("Channel.endsAreNotInheritable", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    for (auto const* channel: { &pair->parent, &pair->helper })
    {
        auto flags = DWORD { 0 };
        REQUIRE(::GetHandleInformation(channel->nativeHandle(), &flags));
        CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
    }
}

TEST_CASE("Channel.transferInFlightOpensNoHandle", "[vtextension]")
{
    // An end holds the same handles whether a receive waits on it or not: what a process holds must not
    // depend on what its threads happen to be waiting for, or a count of its handles races them.
    if (vtextension::wasm::testing::runsOnWine())
        SKIP("wine's GetProcessHandleCount() reads 0, whatever the process holds");
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    auto const handleCount = [] {
        auto count = DWORD { 0 };
        REQUIRE(::GetProcessHandleCount(::GetCurrentProcess(), &count));
        return count;
    };
    // The receiving thread is started, and parked, before the count: a thread holds handles of its own, and
    // std::async's pool may make them only once the thread runs, so the count waits until it has.
    auto started = std::promise<void> {};
    auto running = started.get_future();
    auto go = std::promise<void> {};
    auto received = std::async(std::launch::async, [&, release = go.get_future()] {
        started.set_value();
        release.wait();
        auto buffer = std::array<uint8_t, 1> {};
        return pair->helper.receiveSome(buffer);
    });
    // Before the future is destroyed, whatever fails first: the thread released, and its receive ended, so
    // that the future's destructor, which waits for the thread, never waits on a receive nothing will end.
    auto goSet = false;
    auto const endTheReceive = core::Finally { [&] {
        if (!std::exchange(goSet, true))
            go.set_value();
        pair->helper.shutdown();
    } };
    running.wait();
    auto const before = handleCount();
    goSet = true;
    go.set_value();
    // Looked at while the receive waits: long enough for it to have started, the bytes not sent yet.
    auto most = before;
    for ([[maybe_unused]] auto const look: std::views::iota(0, 200))
    {
        most = std::max(most, handleCount());
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }
    REQUIRE(pair->parent.send(std::array<uint8_t, 1> { 7 }).has_value());
    auto const got = received.get();
    CHECK(got == std::size_t { 1 });
    CHECK(most == before);
}
#endif

TEST_CASE("Channel.adoptTakesAnInheritedEnd", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    auto const copy = duplicate(pair->helper.nativeHandle());
    REQUIRE(copy.has_value());
    auto adopted = Channel::adopt(*copy);
    REQUIRE(adopted.has_value());
    {
        // Only the adopted end is left on the helper's side.
        auto const helper = std::move(pair->helper);
    }

    auto const request = std::vector<uint8_t> { 1, 2, 3 };
    REQUIRE(pair->parent.send(request).has_value());
    CHECK(receiveExactly(*adopted, request.size()) == request);
    auto const reply = std::vector<uint8_t> { 4, 5 };
    REQUIRE(adopted->send(reply).has_value());
    CHECK(receiveExactly(pair->parent, reply.size()) == reply);
#ifndef _WIN32
    // An end handed over is not handed on: close-on-exec again, as dup() left it not.
    auto const flags = ::fcntl(adopted->nativeHandle(), F_GETFD);
    REQUIRE(flags != -1);
    CHECK((flags & FD_CLOEXEC) != 0);
#endif
}

TEST_CASE("Channel.adoptRefusesAClosedHandle", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    auto const copy = duplicate(pair->helper.nativeHandle());
    REQUIRE(copy.has_value());
    closeHandle(*copy);
    // Nothing else of this test opens a handle meanwhile, so the number names nothing.
    CHECK(Channel::adopt(*copy) == std::unexpected(ChannelError::Io));
}

TEST_CASE("Channel.trySendNeverBlocks", "[vtextension]")
{
    using namespace std::chrono_literals;
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());

    // Nobody reads: chunks go in until the channel takes no more, and then trySend() says so rather than
    // wait. Bounded, so that a channel that took everything would end the loop too, and fail below.
    constexpr auto MaxSentBytes = std::size_t { 64 } * 1024 * 1024;
    auto const chunk = std::vector<uint8_t>(std::size_t { 4 } * 1024, uint8_t { 0x3c });
    auto filling = std::async(std::launch::async, [&]() -> std::expected<std::size_t, ChannelError> {
        auto total = std::size_t { 0 };
        while (total < MaxSentBytes)
        {
            auto const sent = pair->parent.trySend(chunk);
            if (!sent)
                return sent;
            if (*sent == 0)
                break;
            total += *sent;
        }
        return total;
    });
    if (filling.wait_for(5s) != std::future_status::ready)
    {
        // The future's destructor would wait for the blocked send forever: the run ends here, rather than
        // hang.
        FAIL_CHECK("trySend() blocked");
        std::fputs("Channel.trySendNeverBlocks: trySend() blocked\n", stderr);
        std::abort();
    }
    auto const filled = filling.get();
    REQUIRE(filled.has_value());
    CHECK(*filled > 0);
    CHECK(*filled < MaxSentBytes);

    // What it took arrives, all of it, and the channel takes more once drained.
    auto const received = receiveExactly(pair->helper, *filled);
    REQUIRE(received.has_value());
    CHECK(std::ranges::all_of(*received, [](uint8_t byte) { return byte == 0x3c; }));
    CHECK(pair->parent.trySend(chunk).value_or(0) > 0);

    pair->helper.shutdown();
    CHECK(pair->parent.trySend(chunk) == std::unexpected(ChannelError::Closed));
}

#ifndef _WIN32
namespace proto = vtextension::wasm::remote::proto;
using vtextension::wasm::remote::transport::OwnedHandle;
using vtextension::wasm::remote::transport::upwardDescriptorsWhenUnrefused;
using vtextension::wasm::remote::transport::testing::forgetToldForeignAncillaryData;
using vtextension::wasm::remote::transport::testing::inodeOf;
using vtextension::wasm::remote::transport::testing::openDescriptorCount;
using vtextension::wasm::remote::transport::testing::receiveSomeClosingAttachments;

namespace
{

/// A pipe's two ends, closed when it is destroyed.
struct Pipe
{
    OwnedHandle reader;
    OwnedHandle writer;
};

/// @return A fresh pipe.
[[nodiscard]] Pipe makePipe()
{
    auto ends = std::array<int, 2> { -1, -1 };
    REQUIRE(::pipe(ends.data()) == 0);
    return Pipe { .reader = OwnedHandle { ends[0] }, .writer = OwnedHandle { ends[1] } };
}

/// @return A connected pair whose ends both take descriptors in, as a system without `SO_PASSRIGHTS` makes
///         them: for the path where the system discards what a helper sends up, which makeChannelPair()'s
///         pairs never reach on a kernel that refuses it.
[[nodiscard]] ChannelPair discardingPair()
{
    auto ends = std::array<int, 2> { -1, -1 };
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0);
    auto parent = Channel::adopt(ends[0]);
    auto helper = Channel::adopt(ends[1]);
    REQUIRE(parent.has_value());
    REQUIRE(helper.has_value());
    return ChannelPair { .parent = *std::move(parent),
                         .helper = *std::move(helper),
                         .upward = UpwardDescriptors::Discarded };
}

} // namespace

TEST_CASE("Channel.handleTravelsWithItsBytes", "[vtextension]")
{
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    auto const pipe = makePipe();
    auto const bytes = std::array<uint8_t, 3> { 1, 2, 3 };
    REQUIRE(pair->parent.sendWithHandle(bytes, pipe.writer.get()).has_value());

    auto received = std::array<uint8_t, 8> {};
    auto handles = std::vector<OwnedHandle> {};
    auto const count = pair->helper.receiveSomeWithHandles(received, handles);
    REQUIRE(count == std::size_t { 3 });
    CHECK(std::ranges::equal(std::span { received }.first(3), bytes));
    REQUIRE(handles.size() == 1);
    // A descriptor of the receiver's own, on what the sender's named, that no child inherits.
    CHECK(handles.front().get() != pipe.writer.get());
    CHECK(inodeOf(handles.front().get()) == inodeOf(pipe.writer.get()));
    auto const flags = ::fcntl(handles.front().get(), F_GETFD);
    REQUIRE(flags != -1);
    CHECK((flags & FD_CLOEXEC) != 0);

    // Plain bytes after it carry nothing.
    REQUIRE(pair->parent.send(bytes).has_value());
    auto none = std::vector<OwnedHandle> {};
    CHECK(pair->helper.receiveSomeWithHandles(received, none) == std::size_t { 3 });
    CHECK(none.empty());
}

    #ifdef __linux__
TEST_CASE("Channel.parentEndRefusesDescriptors", "[vtextension]")
{
    // Contour's end refuses what a helper attaches: the helper's sendmsg fails (EPERM, reported as Refused)
    // with nothing sent, so no descriptor enters Contour's process -- not even to be discarded, a close that
    // could block a thread of Contour's. The helper's end still takes Contour's grants.
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    if (pair->upward != UpwardDescriptors::Refused)
        SKIP("this kernel predates SO_PASSRIGHTS (Linux 6.16)");
    auto const pipe = makePipe();
    auto const bytes = std::array<uint8_t, 3> { 1, 2, 3 };
    CHECK(pair->helper.sendWithHandle(bytes, pipe.writer.get()) == std::unexpected(ChannelError::Refused));

    // Only what is sent after it arrives, and with nothing attached.
    REQUIRE(pair->helper.send(bytes).has_value());
    auto received = std::array<uint8_t, 8> {};
    auto handles = std::vector<OwnedHandle> {};
    CHECK(pair->parent.receiveSomeWithHandles(received, handles) == std::size_t { 3 });
    CHECK(handles.empty());

    REQUIRE(pair->parent.sendWithHandle(bytes, pipe.writer.get()).has_value());
    CHECK(pair->helper.receiveSomeWithHandles(received, handles) == std::size_t { 3 });
    CHECK(handles.size() == 1);
}
    #endif

TEST_CASE("Channel.unrefusableDescriptorsAreDiscarded", "[vtextension]")
{
    // A system without SO_PASSRIGHTS, and one whose outer sandbox or security module refuses it, discard what
    // a helper attaches instead; anything else is a failure of the system.
    for (auto const error: { ENOPROTOOPT, EINVAL, EPERM, EACCES })
        CHECK(upwardDescriptorsWhenUnrefused(error) == UpwardDescriptors::Discarded);
    for (auto const error: { EBADF, ENOMEM, ENOTSOCK })
        CHECK(upwardDescriptorsWhenUnrefused(error) == std::unexpected(ChannelError::Io));
}

TEST_CASE("Channel.parentInstallsNoDescriptorFromTheHelper", "[vtextension]")
{
    // A helper attaches a descriptor to every frame it sends, to an end that does not refuse them (as on a
    // system without SO_PASSRIGHTS); Contour's reader takes in none of them.
    auto pair = discardingPair();
    auto parent = vtextension::wasm::remote::transport::PduStream {
        std::move(pair.parent), proto::ChannelKind::Control, proto::Direction::ToParent, proto::PduLimits {}
    };
    auto const frame = proto::encodeFrame(proto::RuntimeCreated { .runtimeId = 7, .error = std::nullopt });
    REQUIRE(frame.has_value());
    constexpr auto Frames = 100;

    auto const before = openDescriptorCount();
    for ([[maybe_unused]] auto const index: std::views::iota(0, Frames))
    {
        // Sent and received one at a time, so that the socket's buffer never fills.
        {
            auto const pipe = makePipe();
            REQUIRE(pair.helper.sendWithHandle(*frame, pipe.writer.get()).has_value());
        }
        auto const received = parent.receive();
        REQUIRE(received.has_value());
        CHECK(std::holds_alternative<proto::RuntimeCreated>(*received));
    }
    CHECK(openDescriptorCount() == before);
    CHECK_FALSE(parent.takeAttachedHandle());
}
    #ifdef SO_PASSCRED
TEST_CASE("Channel.foreignAncillaryDataIsDiscardedAndToldOnce", "[vtextension]")
{
    // The receive macOS makes, where the system installs what came beside the bytes whether or not it is
    // asked for (testing::receiveSomeClosingAttachments()), run here on any system that has it. Credentials,
    // which the kernel attaches to every message once the reader asks for them, are ancillary data that no
    // peer sends: the bytes still arrive, nothing stays installed -- a descriptor sent along neither -- and
    // the warning log tells of it once, however many such messages come.
    auto pair = makeChannelPair();
    REQUIRE(pair.has_value());
    auto const enable = 1;
    REQUIRE(::setsockopt(pair->helper.nativeHandle(), SOL_SOCKET, SO_PASSCRED, &enable, sizeof(enable)) == 0);

    auto& registry = core::log::get();
    auto const category = std::ranges::find_if(
        registry, [](core::log::Category const& each) { return each.name() == "vtextension.warning"; });
    REQUIRE(category != registry.end());
    auto warnings = std::vector<std::string> {};
    auto sink = core::log::Sink { true, [&](std::string_view const& text) { warnings.emplace_back(text); } };
    auto& previous = category->get().sink();
    category->get().setSink(sink);
    auto const restore = core::Finally { [&] { category->get().setSink(previous); } };
    // The warning is given once per process, and every case of vtextension_test runs in one: forgotten
    // first, so that this case counts its own, whatever ran before it.
    forgetToldForeignAncillaryData();

    auto const before = openDescriptorCount();
    auto const bytes = std::array<uint8_t, 1> { 0x5a };
    auto const receiveOne = [&] {
        auto received = std::array<uint8_t, 1> {};
        auto const count = receiveSomeClosingAttachments(pair->helper, received);
        REQUIRE(count == std::expected<std::size_t, ChannelError> { 1 });
        CHECK(received == bytes);
        CHECK(openDescriptorCount() == before);
    };
    {
        // With a descriptor, which is taken in and closed.
        auto const pipe = makePipe();
        REQUIRE(pair->parent.sendWithHandle(bytes, pipe.writer.get()).has_value());
    }
    receiveOne();
    // Bytes alone, which still come with credentials.
    REQUIRE(pair->parent.send(bytes).has_value());
    receiveOne();
    // Two messages, one warning: it is given once per process (Channel_posix.cpp).
    CHECK(warnings.size() == 1);
}
    #endif
#endif
