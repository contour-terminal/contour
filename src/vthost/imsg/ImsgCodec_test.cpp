// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/imsg/CommandArgv.h>
#include <vthost/imsg/Identify.h>
#include <vthost/imsg/ImsgCodec.h>

using namespace vthost::imsg;

namespace
{

[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    auto const* begin = reinterpret_cast<std::byte const*>(text.data());
    return { begin, begin + text.size() };
}

[[nodiscard]] std::vector<std::byte> cstringOf(std::string_view text)
{
    auto out = bytesOf(text);
    out.push_back(std::byte { 0 });
    return out;
}

/// Applies @p frame and reports success (kept out of the Catch2 macros:
/// their expression decomposition trips use-after-move analysis).
[[nodiscard]] bool applies(IdentifyState& state, ImsgFrame frame)
{
    return applyIdentify(state, std::move(frame)).has_value();
}

/// Applies @p frame and yields the rejection error.
[[nodiscard]] ImsgError applyError(IdentifyState& state, ImsgFrame frame)
{
    return applyIdentify(state, std::move(frame)).error();
}

/// A closer that records instead of closing (the test owns no real fds).
struct RecordingCloser
{
    std::vector<int>* closed;
    void operator()(int fd) const { closed->push_back(fd); }
};

} // namespace

TEST_CASE("the header encodes 16 host-order bytes with len including itself", "[vthost][imsg]")
{
    auto const payload = bytesOf("hi");
    auto const wire = encodeFrame(msgtype::Command, payload, /*hasFd=*/false, /*pid=*/1234);
    REQUIRE(wire.size() == HeaderSize + 2);

    auto type = uint32_t {};
    auto len = uint32_t {};
    auto peerid = uint32_t {};
    auto pid = uint32_t {};
    std::memcpy(&type, wire.data(), 4);
    std::memcpy(&len, wire.data() + 4, 4);
    std::memcpy(&peerid, wire.data() + 8, 4);
    std::memcpy(&pid, wire.data() + 12, 4);
    CHECK(type == 200);
    CHECK(len == HeaderSize + 2);
    CHECK(peerid == ProtocolVersion); // the version rides in peerid's low byte
    CHECK(pid == 1234);
}

TEST_CASE("an empty payload encodes to a bare header and decodes back", "[vthost][imsg]")
{
    // ImsgServer sends a payload-less MSG_VERSION reply on a protocol mismatch;
    // encodeFrame must not memcpy from the empty span's null data pointer (UB
    // even for a zero count, which UBSan flags).
    auto const wire = encodeFrame(msgtype::Version, {});
    REQUIRE(wire.size() == HeaderSize);

    auto len = uint32_t {};
    std::memcpy(&len, wire.data() + 4, 4);
    CHECK(len == HeaderSize); // header only, no fd mark

    auto decoder = ImsgDecoder {};
    decoder.feed(wire);
    auto const frame = decoder.next();
    REQUIRE(frame.has_value());
    REQUIRE(frame->has_value());
    CHECK((*frame)->type == msgtype::Version);
    CHECK((*frame)->payload.empty());
}

TEST_CASE("frames round-trip and resume across byte-at-a-time feeds", "[vthost][imsg]")
{
    auto const wire = encodeFrame(msgtype::IdentifyTerm, cstringOf("xterm-256color"));

    auto decoder = ImsgDecoder {};
    for (auto const byte: wire)
    {
        auto const before = decoder.next();
        REQUIRE(before.has_value()); // never an error mid-frame
        decoder.feed(std::span { &byte, 1 });
    }
    auto const frame = decoder.next();
    REQUIRE(frame.has_value());
    REQUIRE(frame->has_value());
    CHECK((*frame)->type == msgtype::IdentifyTerm);
    CHECK((*frame)->payload == cstringOf("xterm-256color"));
    CHECK(!(*frame)->fd.valid());
}

TEST_CASE("a stream fragmented mid-frame decodes every frame without unbounded buffering",
          "[vthost][imsg]")
{
    // Every chunk deliberately ends mid-frame, so the decoder always holds a partial trailing frame.
    // The old compaction (only when the buffer drained EXACTLY) never fired here, letting the consumed
    // prefix accumulate for the connection's whole life. Compacting on every feed bounds memory; this
    // asserts the harder-to-break half — that the compaction preserves correctness across many frames.
    auto const one = encodeFrame(msgtype::IdentifyTerm, cstringOf("xterm"));
    auto constexpr FrameCount = 500;

    auto stream = std::vector<std::byte> {};
    for ([[maybe_unused]] auto const i: std::views::iota(0, FrameCount))
        stream.insert(stream.end(), one.begin(), one.end());

    auto decoder = ImsgDecoder {};
    auto decoded = 0;
    auto const chunk = one.size() + 1; // one full frame plus a byte: each feed leaves a fragment
    for (std::size_t offset = 0; offset < stream.size(); offset += chunk)
    {
        auto const take = std::min(chunk, stream.size() - offset);
        decoder.feed(std::span { stream.data() + offset, take });
        while (true)
        {
            auto const frame = decoder.next();
            REQUIRE(frame.has_value()); // never a decode error
            if (!frame->has_value())
                break; // needs more bytes
            CHECK((*frame)->type == msgtype::IdentifyTerm);
            CHECK((*frame)->payload == cstringOf("xterm"));
            ++decoded;
        }
    }
    CHECK(decoded == FrameCount);
}

TEST_CASE("out-of-range lengths are fatal", "[vthost][imsg]")
{
    for (auto const badLen: { uint32_t { 4 }, MaxMessageSize + 1 })
    {
        auto wire = encodeFrame(msgtype::Version, {});
        std::memcpy(wire.data() + 4, &badLen, 4);
        auto decoder = ImsgDecoder {};
        decoder.feed(wire);
        auto const result = decoder.next();
        REQUIRE(!result.has_value());
        CHECK(result.error() == ImsgError::LengthOutOfRange);
    }
}

TEST_CASE("a pending fd is claimed by the first marked header", "[vthost][imsg]")
{
    auto closed = std::vector<int> {};
    auto decoder = ImsgDecoder { RecordingCloser { &closed } };

    // An UNMARKED frame first, then a marked one; the fd arrives with the
    // chunk but belongs to the marked frame even across split feeds.
    auto const plain = encodeFrame(msgtype::IdentifyFlags, bytesOf("\0\0\0\0"));
    auto const marked = encodeFrame(msgtype::IdentifyStdin, {}, /*hasFd=*/true);
    decoder.feed(plain, /*fd=*/-1);
    decoder.feed(std::span { marked.data(), (marked.size() / 2) }, /*fd=*/77);

    auto first = decoder.next();
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK(!(*first)->fd.valid()); // the unmarked frame never claims it

    auto const half = marked.size() / 2;
    decoder.feed(std::span { marked.data() + half, marked.size() - half });
    auto second = decoder.next();
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK((*second)->fd.get() == 77);
    CHECK(closed.empty());
    std::ignore = (*second)->fd.release(); // do not "close" the fake fd
}

TEST_CASE("an unclaimed fd is closed when the next one arrives", "[vthost][imsg]")
{
    auto closed = std::vector<int> {};
    {
        auto decoder = ImsgDecoder { RecordingCloser { &closed } };
        decoder.feed({}, 10);
        decoder.feed({}, 11); // replaces (and closes) fd 10
        CHECK(closed == std::vector { 10 });
    }
    // Destruction closes the still-pending fd 11.
    CHECK(closed == std::vector { 10, 11 });
}

TEST_CASE("a marked header without an fd is tolerated", "[vthost][imsg]")
{
    // The EMSGSIZE-lost-fd case: the mark is set, no descriptor arrived.
    auto decoder = ImsgDecoder {};
    decoder.feed(encodeFrame(msgtype::IdentifyStdout, {}, /*hasFd=*/true));
    auto const frame = decoder.next();
    REQUIRE(frame.has_value());
    REQUIRE(frame->has_value());
    CHECK(!(*frame)->fd.valid());
}

TEST_CASE("argv packs and unpacks per cmd_unpack_argv's rules", "[vthost][imsg]")
{
    auto const empty = unpackArgv(packArgv({}));
    REQUIRE(empty.has_value());
    CHECK(empty->empty()); // argc 0 is legal (server default command)

    auto const arguments = std::vector<std::string> { "attach-session", "-t", "my session" };
    auto const round = unpackArgv(packArgv(arguments));
    REQUIRE(round.has_value());
    CHECK(*round == arguments);

    // argc out of range.
    auto huge = packArgv({});
    auto const badArgc = 1001;
    std::memcpy(huge.data(), &badArgc, sizeof(int));
    CHECK(unpackArgv(huge).error() == ImsgError::BadArgv);
    auto negative = packArgv({});
    auto const negArgc = -1;
    std::memcpy(negative.data(), &negArgc, sizeof(int));
    CHECK(unpackArgv(negative).error() == ImsgError::BadArgv);

    // A truncated string list (missing final NUL).
    auto truncated = packArgv(arguments);
    truncated.pop_back();
    CHECK(unpackArgv(truncated).error() == ImsgError::BadArgv);

    CHECK(unpackArgv({}).error() == ImsgError::BadArgv); // shorter than argc itself
}

TEST_CASE("the identify sequence a 3.7b client sends accumulates", "[vthost][imsg]")
{
    auto state = IdentifyState {};
    auto const longFlags = uint64_t { ClientControl | uint64_t { 0x10000 } };
    auto flagsPayload = std::vector<std::byte>(sizeof(uint64_t));
    std::memcpy(flagsPayload.data(), &longFlags, sizeof(uint64_t));

    // LONGFLAGS is sent TWICE by the real client; the second wins (equal).
    for (auto i = 0; i < 2; ++i)
    {
        auto frame = ImsgFrame {};
        frame.type = msgtype::IdentifyLongFlags;
        frame.payload = flagsPayload;
        auto const frameApplied = applies(state, std::move(frame));
        REQUIRE(frameApplied);
    }
    auto term = ImsgFrame {};
    term.type = msgtype::IdentifyTerm;
    term.payload = cstringOf("tmux-256color");
    auto const termApplied = applies(state, std::move(term));
    REQUIRE(termApplied);
    auto environ1 = ImsgFrame {};
    environ1.type = msgtype::IdentifyEnviron;
    environ1.payload = cstringOf("HOME=/home/u");
    auto environ2 = ImsgFrame {};
    environ2.type = msgtype::IdentifyEnviron;
    environ2.payload = cstringOf("no-equals");
    auto const environ1Applied = applies(state, std::move(environ1));
    REQUIRE(environ1Applied);
    auto const environ2Applied = applies(state, std::move(environ2));
    REQUIRE(environ2Applied);
    auto stdinFrame = ImsgFrame {};
    stdinFrame.type = msgtype::IdentifyStdin;
    stdinFrame.fd = UniqueFd { 5, [](int) {} };
    auto const stdinFrameApplied = applies(state, std::move(stdinFrame));
    REQUIRE(stdinFrameApplied);
    auto stdoutFrame = ImsgFrame {};
    stdoutFrame.type = msgtype::IdentifyStdout;
    stdoutFrame.fd = UniqueFd { 6, [](int) {} };
    auto const stdoutFrameApplied = applies(state, std::move(stdoutFrame));
    REQUIRE(stdoutFrameApplied);
    auto unknown = ImsgFrame {};
    unknown.type = 999;
    unknown.payload = bytesOf("future");
    auto const unknownApplied = applies(state, std::move(unknown));
    REQUIRE(unknownApplied); // ignored
    auto done = ImsgFrame {};
    done.type = msgtype::IdentifyDone;
    auto const doneApplied = applies(state, std::move(done));
    REQUIRE(doneApplied);

    CHECK(state.flags == longFlags);
    CHECK(state.term == "tmux-256color");
    CHECK(state.environment == std::vector<std::string> { "HOME=/home/u" });
    CHECK(state.stdinFd.get() == 5);
    CHECK(state.stdoutFd.get() == 6);
    CHECK(state.done);

    // Identify after DONE kills the peer.
    auto late = ImsgFrame {};
    late.type = msgtype::IdentifyTerm;
    late.payload = cstringOf("x");
    auto const lateError = applyError(state, std::move(late));
    CHECK(lateError == ImsgError::IdentifyAfterDone);

    CHECK(checkAcceptance(state).has_value());
}

TEST_CASE("payload-shape violations and the acceptance matrix reject", "[vthost][imsg]")
{
    auto state = IdentifyState {};
    auto unterminated = ImsgFrame {};
    unterminated.type = msgtype::IdentifyTerm;
    unterminated.payload = bytesOf("no-nul");
    auto const unterminatedError = applyError(state, std::move(unterminated));
    CHECK(unterminatedError == ImsgError::MalformedString);
    auto shortFlags = ImsgFrame {};
    shortFlags.type = msgtype::IdentifyLongFlags;
    shortFlags.payload = bytesOf("abc");
    auto const shortFlagsError = applyError(state, std::move(shortFlags));
    CHECK(shortFlagsError == ImsgError::WrongPayloadSize);
    auto nonEmptyStdin = ImsgFrame {};
    nonEmptyStdin.type = msgtype::IdentifyStdin;
    nonEmptyStdin.payload = bytesOf("x");
    auto const nonEmptyStdinError = applyError(state, std::move(nonEmptyStdin));
    CHECK(nonEmptyStdinError == ImsgError::WrongPayloadSize);

    auto const withFds = [](uint64_t flags) {
        auto s = IdentifyState {};
        s.flags = flags;
        s.stdinFd = UniqueFd { 5, [](int) {} };
        s.stdoutFd = UniqueFd { 6, [](int) {} };
        s.done = true;
        return s;
    };
    CHECK(checkAcceptance(withFds(0)).error() == RejectReason::NotControlClient);
    CHECK(checkAcceptance(withFds(ClientControl | ClientControlControl)).error()
          == RejectReason::ControlControl);
    auto noFds = IdentifyState {};
    noFds.flags = ClientControl;
    noFds.done = true;
    CHECK(checkAcceptance(noFds).error() == RejectReason::MissingStdioFds);
    CHECK(!rejectMessage(RejectReason::NotControlClient).empty());
}
