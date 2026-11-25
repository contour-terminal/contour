// SPDX-License-Identifier: Apache-2.0
//
// The extension host protocol's codec: every kind round-trips, and every frame a peer could make up is
// refused for the right reason, from as few bytes as the reason needs.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/PduTable.hpp>

using namespace vtextension::wasm::remote::proto;
using vtextension::ExtensionError;
using vtextension::wasm::CallFailure;
using vtextension::wasm::ExportDescription;
using vtextension::wasm::ExternKind;
using vtextension::wasm::ImportDescription;
using vtextension::wasm::Linkage;
using vtextension::wasm::ValueType;

namespace
{

/// @return @p values as bytes.
[[nodiscard]] std::vector<std::byte> raw(std::initializer_list<uint8_t> values)
{
    return std::ranges::to<std::vector<std::byte>>(
        values | std::views::transform([](uint8_t value) { return static_cast<std::byte>(value); }));
}

/// @return One populated PDU of every kind, in kind order.
[[nodiscard]] std::vector<Pdu> samples()
{
    return {
        Hello { .version = ProtocolVersion,
                .memoryMode = MemoryMode::Shared,
                .lockdown = LockdownState::LockedDown },
        CreateRuntime {
            .runtimeId = 7,
            .memoryBytes = uint64_t { 32 } * 1024 * 1024,
            .stackBytes = uint64_t { 128 } * 1024,
            .functions = { WireHostFunction {
                               .module = "contour:ext/host@0", .name = "log", .parameterCount = 3 },
                           WireHostFunction { .module = "wasi", .name = "x", .parameterCount = 0 } },
        },
        RuntimeCreated { .runtimeId = 7, .error = ExtensionError::RuntimeUnavailable },
        Load { .runtimeId = 7, .moduleId = 9, .bytes = raw({ 0x00, 0x61, 0x73, 0x6d, 0xff }) },
        Loaded {
            .moduleId = 9,
            .error = std::nullopt,
            .imports = { ImportDescription { .module = "m",
                                             .name = "f",
                                             .kind = ExternKind::Function,
                                             .signature = { .parameters = { ValueType::I32, ValueType::I64 },
                                                            .results = { ValueType::F32 } },
                                             .linkage = Linkage::Satisfied },
                         ImportDescription { .module = "m",
                                             .name = "mem",
                                             .kind = ExternKind::Memory,
                                             .signature = {},
                                             .linkage = Linkage::Unsatisfied } },
            .exports = { ExportDescription { .name = "add",
                                             .kind = ExternKind::Function,
                                             .signature = { .parameters = { ValueType::I32, ValueType::I32 },
                                                            .results = { ValueType::I32 } },
                                             .initialPages = 0 },
                         ExportDescription { .name = "memory",
                                             .kind = ExternKind::Memory,
                                             .signature = {},
                                             .initialPages = 17 } },
        },
        Instantiate { .moduleId = 9, .instanceId = 11 },
        Instantiated { .instanceId = 11, .error = std::nullopt, .callableExports = { 0, 3, 70000 } },
        Release { .target = ReleaseTarget::Runtime, .id = 7 },
        Call { .instanceId = 11, .exportOrdinal = 2, .argv = { 1, 0xFFFFFFFF, 300 } },
        CallHost { .functionIndex = 4, .arguments = { 1, 2, 3 } },
        HostResult { .error = ExtensionError::PermissionDenied },
        CallResult { .failure = CallFailure::HostFailure, .argv = { 42 } },
        MemoryRead { .instanceId = 11, .address = 0x10000, .size = 256 },
        MemoryData { .status = MemoryStatus::Ok, .bytes = raw({ 1, 2, 3 }) },
        MemoryWrite { .instanceId = 11, .address = 0xFFFFFFF0, .bytes = raw({ 9, 8 }) },
    };
}

/// @return The table row of @p pdu's kind.
[[nodiscard]] PduRow const& rowOf(Pdu const& pdu)
{
    return PduTable.at(std::to_underlying(kindOf(pdu)) - 1);
}

/// @return @p value as a varint.
[[nodiscard]] std::vector<uint8_t> varint(uint64_t value)
{
    auto writer = Writer {};
    writer.varint(value);
    return writer.take();
}

/// @return A frame of kind @p kind around @p body, framed as the codec frames.
[[nodiscard]] std::vector<uint8_t> frameOf(uint64_t kind, std::span<uint8_t const> body)
{
    auto const tag = varint(kind);
    auto frame = varint(tag.size() + body.size());
    frame.insert(frame.end(), tag.begin(), tag.end());
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

/// @return Why @p frame decodes to nothing as a @p row PDU; nothing if it decodes.
[[nodiscard]] std::optional<DecodeError> errorOf(std::span<uint8_t const> frame, PduRow const& row)
{
    auto const decoded = decodeFrame(frame, row.direction, row.channel, PduLimits {});
    if (decoded)
        return std::nullopt;
    return decoded.error();
}

} // namespace

TEST_CASE("PduCodec.tableHasOneRowPerKind", "[vtextension]")
{
    CHECK(PduTable.size() == 15);
    for (auto const index: std::views::iota(std::size_t { 0 }, PduTable.size()))
    {
        INFO("row " << index);
        CHECK(PduTable[index].kind == static_cast<PduKind>(index + 1));
    }
}

TEST_CASE("PduCodec.roundTrip", "[vtextension]")
{
    for (auto const& sample: samples())
    {
        DYNAMIC_SECTION("kind " << static_cast<int>(kindOf(sample)))
        {
            auto const& row = rowOf(sample);
            auto const frame = encodeFrame(sample);
            auto const decoded = decodeFrame(frame, row.direction, row.channel, PduLimits {});
            REQUIRE(decoded.has_value());
            CHECK(decoded->pdu == sample);
            CHECK(decoded->consumed == frame.size());
        }
    }
}

TEST_CASE("PduCodec.decodesOneFrameOffAStream", "[vtextension]")
{
    auto const first = Pdu { Call { .instanceId = 1, .exportOrdinal = 2, .argv = { 3 } } };
    auto const second = Pdu { MemoryRead { .instanceId = 4, .address = 5, .size = 6 } };
    auto stream = encodeFrame(first);
    auto const firstSize = stream.size();
    auto const tail = encodeFrame(second);
    stream.insert(stream.end(), tail.begin(), tail.end());

    auto const decoded = decodeFrame(stream, Direction::ToHelper, ChannelKind::Lane, PduLimits {});
    REQUIRE(decoded.has_value());
    CHECK(decoded->pdu == first);
    CHECK(decoded->consumed == firstSize);
}

TEST_CASE("PduCodec.everyPrefixNeedsMoreData", "[vtextension]")
{
    for (auto const& sample: samples())
    {
        auto const& row = rowOf(sample);
        auto const frame = encodeFrame(sample);
        for (auto const size: std::views::iota(std::size_t { 0 }, frame.size()))
        {
            INFO("kind " << static_cast<int>(row.kind) << ", " << size << " of " << frame.size() << " bytes");
            CHECK(errorOf(std::span { frame }.first(size), row) == DecodeError::NeedMoreData);
        }
    }
}

TEST_CASE("PduCodec.overCapIsRejectedFromTheHeader", "[vtextension]")
{
    for (auto const& row: PduTable)
    {
        INFO("kind " << static_cast<int>(row.kind));
        // The length counts the kind's byte, then one body byte more than the cap allows; no body follows.
        auto header = varint(1 + row.sizeCap(PduLimits {}) + 1);
        header.push_back(std::to_underlying(row.kind));
        CHECK(errorOf(header, row) == DecodeError::OverCap);

        auto const atCap = [&] {
            auto frame = varint(1 + row.sizeCap(PduLimits {}));
            frame.push_back(std::to_underlying(row.kind));
            return frame;
        }();
        CHECK(errorOf(atCap, row) == DecodeError::NeedMoreData);
    }

    // A length beyond every kind's cap is refused before the kind arrives.
    auto const largest = std::ranges::max(
        PduTable | std::views::transform([](PduRow const& row) { return row.sizeCap(PduLimits {}); }));
    auto const lengthOnly = varint(largest + MaxVarint32Bytes + 1);
    CHECK(errorOf(lengthOnly, PduTable.front()) == DecodeError::OverCap);

    // The stream's limits move the caps that follow them.
    auto const small = PduLimits { .maxModuleBytes = 100, .maxMemoryBytes = 200 };
    auto const load =
        encodeFrame(Load { .runtimeId = 1, .moduleId = 2, .bytes = std::vector<std::byte>(101) });
    auto const& loadRow = PduTable.at(std::to_underlying(PduKind::Load) - 1);
    CHECK(decodeFrame(load, loadRow.direction, loadRow.channel, small).has_value());
    auto const tooLarge =
        encodeFrame(Load { .runtimeId = 1, .moduleId = 2, .bytes = std::vector<std::byte>(200) });
    CHECK(decodeFrame(tooLarge, loadRow.direction, loadRow.channel, small).error() == DecodeError::OverCap);
}

TEST_CASE("PduCodec.lyingBodyIsMalformed", "[vtextension]")
{
    auto const& helloRow = PduTable.at(std::to_underlying(PduKind::Hello) - 1);

    SECTION("an inner count beyond the body is Truncated")
    {
        // Load: runtime 1, module 2, then a hundred bytes claimed and three present.
        auto const body = std::vector<uint8_t> { 1, 2, 100, 0xaa, 0xbb, 0xcc };
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Load), body),
                      PduTable.at(std::to_underlying(PduKind::Load) - 1))
              == DecodeError::Truncated);
        // Call: instance 1, export 2, then three values claimed and one present.
        auto const call = std::vector<uint8_t> { 1, 2, 3, 7 };
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Call), call),
                      PduTable.at(std::to_underlying(PduKind::Call) - 1))
              == DecodeError::Truncated);
    }

    SECTION("a byte after the last field is TrailingBytes")
    {
        auto const body = std::vector<uint8_t> { 1, 0, 2, 0 };
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Hello), body), helloRow)
              == DecodeError::TrailingBytes);
    }

    SECTION("an enumeration byte out of range is BadEnum")
    {
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Hello), std::vector<uint8_t> { 1, 2, 2 }), helloRow)
              == DecodeError::BadEnum);
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Hello), std::vector<uint8_t> { 1, 0, 3 }), helloRow)
              == DecodeError::BadEnum);
        // A status names the error + 1; one past the last error is out of range.
        auto const lastError = std::to_underlying(ExtensionError::ModuleTooLarge);
        auto const& createdRow = PduTable.at(std::to_underlying(PduKind::RuntimeCreated) - 1);
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::RuntimeCreated),
                              std::vector<uint8_t> { 7, static_cast<uint8_t>(lastError + 1) }),
                      createdRow)
              == std::nullopt);
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::RuntimeCreated),
                              std::vector<uint8_t> { 7, static_cast<uint8_t>(lastError + 2) }),
                      createdRow)
              == DecodeError::BadEnum);
    }

    SECTION("a kind naming no PDU is UnknownKind")
    {
        CHECK(errorOf(frameOf(0, {}), helloRow) == DecodeError::UnknownKind);
        CHECK(errorOf(frameOf(16, {}), helloRow) == DecodeError::UnknownKind);
    }

    SECTION("an over-long varint is MalformedVarint")
    {
        // A u32 field may take five bytes, and its fifth only four bits.
        auto const sixBytes = std::vector<uint8_t> { 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0, 0 };
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Hello), sixBytes), helloRow)
              == DecodeError::MalformedVarint);
        auto const beyond32Bits = std::vector<uint8_t> { 0xff, 0xff, 0xff, 0xff, 0x1f, 0, 0 };
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Hello), beyond32Bits), helloRow)
              == DecodeError::MalformedVarint);
        auto const largest = std::vector<uint8_t> { 0xff, 0xff, 0xff, 0xff, 0x0f, 0, 0 };
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Hello), largest), helloRow) == std::nullopt);
    }

    SECTION("more values than a call carries is OverCap")
    {
        auto const& callRow = PduTable.at(std::to_underlying(PduKind::Call) - 1);
        auto const call = encodeFrame(
            Call { .instanceId = 1, .exportOrdinal = 0, .argv = std::vector<uint32_t>(MaxCallValues + 1) });
        CHECK(errorOf(call, callRow) == DecodeError::OverCap);
        auto const atMost = encodeFrame(
            Call { .instanceId = 1, .exportOrdinal = 0, .argv = std::vector<uint32_t>(MaxCallValues) });
        CHECK(errorOf(atMost, callRow) == std::nullopt);

        auto const& hostRow = PduTable.at(std::to_underlying(PduKind::CallHost) - 1);
        auto const host = encodeFrame(
            CallHost { .functionIndex = 0, .arguments = std::vector<uint32_t>(MaxHostParameters + 1) });
        CHECK(errorOf(host, hostRow) == DecodeError::OverCap);
    }
}

TEST_CASE("PduCodec.directionAndChannelAreChecked", "[vtextension]")
{
    auto const call = encodeFrame(Call { .instanceId = 1, .exportOrdinal = 2, .argv = {} });
    CHECK(decodeFrame(call, Direction::ToParent, ChannelKind::Lane, PduLimits {}).error()
          == DecodeError::WrongDirection);
    auto const load = encodeFrame(Load { .runtimeId = 1, .moduleId = 2, .bytes = {} });
    CHECK(decodeFrame(load, Direction::ToHelper, ChannelKind::Lane, PduLimits {}).error()
          == DecodeError::WrongChannel);
}

TEST_CASE("PduCodec.mutatedFramesNeverCrash", "[vtextension]")
{
    auto const all = samples();
    auto const frames =
        std::ranges::to<std::vector<std::vector<uint8_t>>>(all | std::views::transform(encodeFrame));
    constexpr auto Seeds = std::array { uint32_t { 0x5eed } };
    for (auto const seed: Seeds)
    {
        INFO("seed " << seed);
        // std::mt19937's output is fully specified, unlike the distributions', so every standard library
        // mutates alike.
        auto random = std::mt19937 { seed };
        auto const pick = [&random](std::size_t bound) {
            return static_cast<std::size_t>(random() % bound);
        };
        auto decodes = std::size_t { 0 };
        for (auto const iteration: std::views::iota(0, 10'000))
        {
            auto const index = pick(frames.size());
            auto frame = frames[index];
            switch (pick(3))
            {
                case 0: // Flips up to four bytes to anything.
                    for ([[maybe_unused]] auto const flip: std::views::iota(std::size_t { 0 }, 1 + pick(4)))
                        frame[pick(frame.size())] = static_cast<uint8_t>(pick(256));
                    break;
                case 1: // Cuts the frame short.
                    frame.resize(pick(frame.size()));
                    break;
                default: // Splices the tail of another frame onto a prefix of this one.
                {
                    auto const& other = frames[pick(frames.size())];
                    frame.resize(pick(frame.size()) + 1);
                    frame.insert(frame.end(),
                                 other.begin() + static_cast<std::ptrdiff_t>(pick(other.size())),
                                 other.end());
                    break;
                }
            }
            auto const& row = rowOf(all[index]);
            auto const decoded = decodeFrame(frame, row.direction, row.channel, PduLimits {});
            if (!decoded)
                continue;
            // What decodes is a frame of its own: it encodes again to what decodes alike.
            ++decodes;
            INFO("iteration " << iteration);
            CHECK(decoded->consumed <= frame.size());
            auto const redecoded =
                decodeFrame(encodeFrame(decoded->pdu), row.direction, row.channel, PduLimits {});
            REQUIRE(redecoded.has_value());
            CHECK(redecoded->pdu == decoded->pdu);
        }
        // Byte flips in a field's value leave many frames valid; a run that decoded none tested nothing.
        CHECK(decodes > 0);
    }
}
