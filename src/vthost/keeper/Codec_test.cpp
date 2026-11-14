// SPDX-License-Identifier: Apache-2.0
#include <core/codec/Stream.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <variant>
#include <vector>

#include <vthost/keeper/Codec.hpp>

using namespace vthost::keeper;

namespace
{

/// One sample of each alternative of Message, every field set to something other than its default.
std::vector<Message> samples()
{
    auto const blob = std::vector<uint8_t> { 1, 2, 3, 250 };
    return {
        Hello { .version = 2, .capabilities = 0xDEAD'BEEF'0000ULL },
        Unsupported { .tag = 99 },
        Launch {
            .continuity = LaunchContinuity::HandedOver, .options = blob, .originalDescriptorLimit = 1024 },
        Listener { .name = "native", .tlsIdentity = blob, .path = "/run/contour/native" },
        Serving { .hungRestart = HungRestart::Off, .heartbeatTimeoutMs = 12'345 },
        Spawn { .request = 7,
                .argv = { "/bin/sh", "-l" },
                .executablePath = "/bin/sh",
                .childFds = { 0, 1, 2, 5 },
                .kept = { KeptDescriptor::Master, KeptDescriptor::Settle },
                .executable = ExecutableSource::Descriptor },
        Spawned { .request = 7, .session = 42, .pid = 31'337 },
        SpawnFailed { .request = 8, .error = SpawnError::TooManyDescriptors },
        Placed { .session = 42 },
        Release { .session = 42, .graceMs = 500 },
        Exited { .session = 42, .status = { .kind = ExitKind::Signal, .value = 9 } },
        Structure {},
        Heartbeat {},
        Adopt {},
        Shutdown { .cause = 3 },
        Exec { .executablePath = "/usr/bin/contour" },
        InventoryBegin { .daemonImagePath = "/img", .sessionCount = 4, .listenerCount = 2 },
        InventoryListener { .name = "tls", .tlsIdentity = std::nullopt, .path = {} },
        InventorySession { .session = 5,
                           .pid = -1,
                           .exited = WireExitStatus { .kind = ExitKind::Normal, .value = -3 },
                           .placed = Placement::Parked,
                           .descriptors = { KeptDescriptor::Master, KeptDescriptor::ProcessHandle } },
        InventoryEnd {},
        Signal { .session = 42, .signal = ChildSignal::Kill },
        ShutdownNoted {},
        ExecNoted {},
        ExecAborted {},
    };
}

} // namespace

TEST_CASE("every keeper message round-trips", "[keeper][codec]")
{
    auto const all = samples();
    REQUIRE(all.size() == std::variant_size_v<Message>);
    for (auto const index: std::views::iota(size_t { 0 }, all.size()))
    {
        DYNAMIC_SECTION("alternative " << index)
        {
            REQUIRE(all[index].index() == index); // the table's order is the wire order
            auto const bytes = encode(all[index]);
            auto const decoded = decode(bytes);
            REQUIRE(decoded.has_value());
            CHECK(*decoded == all[index]);
        }
    }
}

TEST_CASE("unknown trailing fields are skipped", "[keeper][codec]")
{
    auto bytes = encode(Hello { .version = 3, .capabilities = 5 });
    // A newer writer's extra fields: a varint field (id 40) and a framed one (id 41).
    auto writer = core::codec::Writer {};
    writer.varint((40U << 1U) | 0U);
    writer.varint(123);
    writer.varint((41U << 1U) | 1U);
    writer.bytes(std::vector<uint8_t> { 9, 9, 9 });
    auto const extra = std::move(writer).take();
    bytes.insert(bytes.end(), extra.begin(), extra.end());

    auto const decoded = decode(bytes);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == Message { Hello { .version = 3, .capabilities = 5 } });
}

TEST_CASE("an unknown tag decodes as UnknownTag, not a misread", "[keeper][codec]")
{
    auto const past = std::variant_size_v<Message> + 1;
    for (auto const tag: { uint64_t { 0 }, uint64_t { past }, uint64_t { 4000 } })
    {
        auto writer = core::codec::Writer {};
        writer.varint(tag);
        auto const bytes = std::move(writer).take();
        auto const decoded = decode(bytes);
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error() == CodecError::UnknownTag);
    }
}

TEST_CASE("a cut body decodes as an error, never as a message", "[keeper][codec]")
{
    CHECK(decode({}).error() == CodecError::Truncated);
    auto const bytes = encode(Exec { .executablePath = "/usr/bin/contour" });
    auto const cut = std::span { bytes }.first(bytes.size() - 3);
    CHECK(!decode(cut).has_value());
}

TEST_CASE("only a body cut short breaks the link; what a newer peer sends is answered Unsupported",
          "[keeper][codec]")
{
    CHECK(failureOf(CodecError::Truncated) == DecodeFailure::Broken);
    for (auto const error: { CodecError::UnknownTag, CodecError::Malformed, CodecError::TooManyDescriptors })
        CHECK(failureOf(error) == DecodeFailure::Unsupported);

    auto body = encode(SpawnFailed { .request = 1, .error = SpawnError::ForkFailed });
    REQUIRE(body.back() == 1); // the error's wire index, last
    body.back() = 9;           // a value a later version appended to the table
    auto const decoded = decode(body);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(failureOf(decoded.error()) == DecodeFailure::Unsupported);
}

TEST_CASE("Hello negotiates the lower version and is never refused", "[keeper][codec]")
{
    CHECK(negotiate(1, 2) == 1);
    CHECK(negotiate(2, 1) == 1);
    CHECK(negotiate(3, 3) == 3);
}

TEST_CASE("no message carries more descriptors than MaxFdsPerFrame", "[keeper][codec]")
{
    CHECK(MaxDescriptorsOfMessage.size() == std::variant_size_v<Message>);
    for (auto const count: MaxDescriptorsOfMessage)
        CHECK(count <= MaxFdsPerFrame);
}

TEST_CASE("WireExitStatus converts to and from vtpty::ExitStatus", "[keeper][codec]")
{
    auto const normal = WireExitStatus::from(vtpty::NormalExit { .exitCode = 3 });
    CHECK(normal == WireExitStatus { .kind = ExitKind::Normal, .value = 3 });
    CHECK(std::get<vtpty::NormalExit>(normal.toExitStatus()).exitCode == 3);
    auto const signalled = WireExitStatus::from(vtpty::SignalExit { .signum = 11 });
    CHECK(signalled == WireExitStatus { .kind = ExitKind::Signal, .value = 11 });
    CHECK(std::get<vtpty::SignalExit>(signalled.toExitStatus()).signum == 11);
}

TEST_CASE("a frame with more descriptors than its message may carry is refused", "[keeper][codec]")
{
    auto const heartbeat = Frame { .body = encode(Heartbeat {}), .fds = { 4 } };
    CHECK(decodeFrame(heartbeat).error() == CodecError::TooManyDescriptors);

    auto const listener =
        Frame { .body = encode(Listener { .name = "x", .tlsIdentity = std::nullopt, .path = {} }),
                .fds = { 4 } };
    CHECK(decodeFrame(listener).has_value());
    auto const two =
        Frame { .body = encode(Listener { .name = "x", .tlsIdentity = std::nullopt, .path = {} }),
                .fds = { 4, 5 } };
    CHECK(decodeFrame(two).error() == CodecError::TooManyDescriptors);

    auto const none =
        Frame { .body = encode(Listener { .name = "x", .tlsIdentity = std::nullopt, .path = {} }),
                .fds = {} };
    CHECK(decodeFrame(none).has_value()); // a budget is a maximum
}

TEST_CASE("peekTag reads the tag of a body, known or not", "[keeper][codec]")
{
    CHECK(peekTag(encode(Hello {})) == 1);
    CHECK(peekTag(encode(Signal {})) == 21);
    CHECK(peekTag(encode(ShutdownNoted {})) == 22);
    CHECK(peekTag(encode(ExecNoted {})) == 23);
    CHECK(peekTag(encode(ExecAborted {})) == std::variant_size_v<Message>);
    CHECK(peekTag(encode(ExecAborted {})) == 24);
    auto writer = core::codec::Writer {};
    writer.varint(4000);
    auto const unknown = std::move(writer).take();
    CHECK(peekTag(unknown) == 4000);
    CHECK(!peekTag({}).has_value());
}
