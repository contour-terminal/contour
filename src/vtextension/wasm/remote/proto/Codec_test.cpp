// SPDX-License-Identifier: Apache-2.0
//
// The extension host protocol's codec: every kind round-trips, and every frame a peer could make up is
// refused for the right reason, from as few bytes as the reason needs.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string_view>
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
            .tableElements = 64,
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
        Instantiate { .moduleId = 9, .instanceId = 11, .interruptSlot = 1023, .memory = {} },
        Instantiate { .moduleId = 9,
                      .instanceId = 12,
                      .interruptSlot = 0,
                      .memory = MemoryGrant { .handle = 0x1F4,
                                              .objectBytes = memoryObjectBytes(uint64_t { 32 } * 1024 * 1024),
                                              .initialBytes = uint64_t { 17 } * WasmPageBytes } },
        Instantiated { .instanceId = 11, .error = std::nullopt, .callableExports = { 0, 3, 70000 } },
        Release { .target = ReleaseTarget::Runtime, .id = 7 },
        Call { .instanceId = 11, .exportOrdinal = 2, .argv = { 1, 0xFFFFFFFF, 300 } },
        CallHost { .functionIndex = 4, .arguments = { 1, 2, 3 } },
        HostResult { .error = ExtensionError::PermissionDenied },
        CallResult { .failure = CallFailure::HostFailure, .argv = { 42 } },
        CallResult { .failure = CallFailure::HostCrashed, .argv = {} },
        HostResult { .error = ExtensionError::HostUnavailable },
        MemoryRead { .instanceId = 11, .address = 0x10000, .size = 256 },
        MemoryData { .status = MemoryStatus::Ok, .bytes = raw({ 1, 2, 3 }) },
        MemoryWrite { .instanceId = 11, .address = 0xFFFFFFF0, .bytes = raw({ 9, 8 }) },
        Grow { .instanceId = 11, .bytes = uint64_t { 18 } * WasmPageBytes },
        Grown { .status = GrowStatus::Granted },
        Grown { .status = GrowStatus::Refused },
    };
}

/// @return @p pdu's frame, which the test expects to encode.
[[nodiscard]] std::vector<uint8_t> encoded(Pdu const& pdu)
{
    auto frame = encodeFrame(pdu);
    REQUIRE(frame.has_value());
    return std::move(*frame);
}

/// @return The table row of @p pdu's kind.
[[nodiscard]] PduRow const& rowOf(Pdu const& pdu)
{
    return PduTable.at(std::to_underlying(kindOf(pdu)) - 1);
}

/// @return How many bytes the body of @p frame spans: its length less its kind's one byte.
[[nodiscard]] uint64_t bodyBytesOf(std::span<uint8_t const> frame)
{
    auto header = Reader { frame };
    auto const length = header.varint64();
    REQUIRE(length.has_value());
    return *length - 1;
}

/// @return The channels a PDU of @p row's kind travels on.
[[nodiscard]] std::vector<ChannelKind> channelsOf(PduRow const& row)
{
    auto channels = std::vector<ChannelKind> {};
    for (auto const channel: { ChannelKind::Control, ChannelKind::Lane })
        if (travelsOn(row, channel))
            channels.push_back(channel);
    return channels;
}

/// @return The first channel a PDU of @p row's kind travels on.
[[nodiscard]] ChannelKind homeChannelOf(PduRow const& row)
{
    auto const channels = channelsOf(row);
    REQUIRE_FALSE(channels.empty());
    return channels.front();
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
    auto const decoded = decodeFrame(frame, row.direction, homeChannelOf(row), PduLimits {});
    if (decoded)
        return std::nullopt;
    return decoded.error();
}

} // namespace

TEST_CASE("PduCodec.tableHasOneRowPerKind", "[vtextension]")
{
    CHECK(PduTable.size() == 17);
    for (auto const index: std::views::iota(std::size_t { 0 }, PduTable.size()))
    {
        INFO("row " << index);
        CHECK(PduTable[index].kind == static_cast<PduKind>(index + 1));
    }
}

TEST_CASE("PduCodec.roundTrip", "[vtextension]")
{
    auto const all = samples();
    for (auto const index: std::views::iota(std::size_t { 0 }, all.size()))
    {
        // Indexed, as a kind may be sampled more than once and a section runs once per name.
        auto const& sample = all[index];
        DYNAMIC_SECTION("sample " << index << ", kind " << static_cast<int>(kindOf(sample)))
        {
            auto const& row = rowOf(sample);
            auto const frame = encoded(sample);
            for (auto const channel: channelsOf(row))
            {
                INFO("channel " << static_cast<int>(channel));
                auto const decoded = decodeFrame(frame, row.direction, channel, PduLimits {});
                REQUIRE(decoded.has_value());
                CHECK(decoded->pdu == sample);
                CHECK(decoded->consumed == frame.size());
            }
        }
    }
}

TEST_CASE("PduCodec.decodesOneFrameOffAStream", "[vtextension]")
{
    auto const first = Pdu { Call { .instanceId = 1, .exportOrdinal = 2, .argv = { 3 } } };
    auto const second = Pdu { MemoryRead { .instanceId = 4, .address = 5, .size = 6 } };
    auto stream = encoded(first);
    auto const firstSize = stream.size();
    auto const tail = encoded(second);
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
        auto const frame = encoded(sample);
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
    auto const load = encoded(Load { .runtimeId = 1, .moduleId = 2, .bytes = std::vector<std::byte>(101) });
    auto const& loadRow = PduTable.at(std::to_underlying(PduKind::Load) - 1);
    CHECK(decodeFrame(load, loadRow.direction, homeChannelOf(loadRow), small).has_value());
    auto const tooLarge =
        encoded(Load { .runtimeId = 1, .moduleId = 2, .bytes = std::vector<std::byte>(200) });
    CHECK(decodeFrame(tooLarge, loadRow.direction, homeChannelOf(loadRow), small).error()
          == DecodeError::OverCap);
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
        auto const& grownRow = PduTable.at(std::to_underlying(PduKind::Grown) - 1);
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Grown), std::vector<uint8_t> { 1 }), grownRow)
              == std::nullopt);
        CHECK(errorOf(frameOf(std::to_underlying(PduKind::Grown), std::vector<uint8_t> { 2 }), grownRow)
              == DecodeError::BadEnum);
        // A status names the error + 1; one past the last error is out of range.
        auto const lastError = std::to_underlying(ExtensionError::HostUnavailable);
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
        CHECK(errorOf(frameOf(PduTable.size() + 1, {}), helloRow) == DecodeError::UnknownKind);
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
        // instance 1, export 0, then the count and as many zero values.
        auto const callOf = [](std::size_t count) {
            auto body = std::vector<uint8_t> { 1, 0, static_cast<uint8_t>(count) };
            body.resize(body.size() + count, 0);
            return frameOf(std::to_underlying(PduKind::Call), body);
        };
        auto const& callRow = PduTable.at(std::to_underlying(PduKind::Call) - 1);
        CHECK(errorOf(callOf(MaxCallValues + 1), callRow) == DecodeError::OverCap);
        CHECK(errorOf(callOf(MaxCallValues), callRow) == std::nullopt);
    }
}

TEST_CASE("PduCodec.encoderRefusesWhatAReaderWould", "[vtextension]")
{
    // A list one past its cap is not encoded at all; at its cap, it is, and decodes.
    struct Case
    {
        std::string_view description;
        Pdu overCap;
        Pdu atCap;
    };
    auto const signatureOf = [](std::size_t parameters) {
        return Loaded { .moduleId = 1,
                        .error = std::nullopt,
                        .imports = { ImportDescription {
                            .module = "m",
                            .name = "f",
                            .kind = ExternKind::Function,
                            .signature = { .parameters = std::vector<ValueType>(parameters, ValueType::I32),
                                           .results = {} },
                            .linkage = Linkage::Satisfied } },
                        .exports = {} };
    };
    auto const cases = std::vector<Case> {
        { .description = "call values",
          .overCap =
              Call { .instanceId = 1, .exportOrdinal = 0, .argv = std::vector<uint32_t>(MaxCallValues + 1) },
          .atCap =
              Call { .instanceId = 1, .exportOrdinal = 0, .argv = std::vector<uint32_t>(MaxCallValues) } },
        { .description = "host call arguments",
          .overCap =
              CallHost { .functionIndex = 0, .arguments = std::vector<uint32_t>(MaxHostParameters + 1) },
          .atCap = CallHost { .functionIndex = 0, .arguments = std::vector<uint32_t>(MaxHostParameters) } },
        { .description = "call results",
          .overCap = CallResult { .failure = std::nullopt, .argv = std::vector<uint32_t>(MaxCallValues + 1) },
          .atCap = CallResult { .failure = std::nullopt, .argv = std::vector<uint32_t>(MaxCallValues) } },
        { .description = "imports",
          .overCap = Loaded { .moduleId = 1,
                              .error = std::nullopt,
                              .imports = std::vector<ImportDescription>(MaxImports + 1),
                              .exports = {} },
          .atCap = Loaded { .moduleId = 1,
                            .error = std::nullopt,
                            .imports = std::vector<ImportDescription>(MaxImports),
                            .exports = {} } },
        { .description = "a signature, nested in an import",
          .overCap = signatureOf(MaxSignatureValues + 1),
          .atCap = signatureOf(MaxSignatureValues) },
        { .description = "callable exports",
          .overCap = Instantiated { .instanceId = 1,
                                    .error = std::nullopt,
                                    .callableExports = std::vector<uint32_t>(MaxExports + 1) },
          .atCap = Instantiated { .instanceId = 1,
                                  .error = std::nullopt,
                                  .callableExports = std::vector<uint32_t>(MaxExports) } },
        { .description = "host functions",
          .overCap = CreateRuntime { .runtimeId = 1,
                                     .memoryBytes = 0,
                                     .stackBytes = 0,
                                     .functions = std::vector<WireHostFunction>(MaxHostFunctions + 1) },
          .atCap = CreateRuntime { .runtimeId = 1,
                                   .memoryBytes = 0,
                                   .stackBytes = 0,
                                   .functions = std::vector<WireHostFunction>(MaxHostFunctions) } },
    };
    for (auto const& [description, overCap, atCap]: cases)
    {
        INFO(description);
        CHECK(encodeFrame(overCap) == std::unexpected(EncodeError::OverCap));
        auto const frame = encodeFrame(atCap);
        REQUIRE(frame.has_value());
        auto const& row = rowOf(atCap);
        auto const decoded = decodeFrame(*frame, row.direction, homeChannelOf(row), PduLimits {});
        REQUIRE(decoded.has_value());
        CHECK(decoded->pdu == atCap);
    }
}

TEST_CASE("PduCodec.lyingCountsAreRefusedBeforeReading", "[vtextension]")
{
    // Each list's count is checked against its cap before an element is read: a count one past the cap is
    // OverCap from a body of a few bytes, where at the cap the same body runs short (Truncated).
    struct Case
    {
        std::string_view description;
        PduKind kind;
        std::vector<uint64_t> prefix; ///< The varints before the count: ids, statuses, enums.
        std::size_t cap;              ///< The count's cap.
    };
    auto const cases = std::vector<Case> {
        { .description = "imports", .kind = PduKind::Loaded, .prefix = { 1, 0 }, .cap = MaxImports },
        { .description = "exports", .kind = PduKind::Loaded, .prefix = { 1, 0, 0 }, .cap = MaxExports },
        // One import, of module "", name "", kind Function, then its parameters' count.
        { .description = "signature parameters",
          .kind = PduKind::Loaded,
          .prefix = { 1, 0, 1, 0, 0, 0 },
          .cap = MaxSignatureValues },
        { .description = "signature results",
          .kind = PduKind::Loaded,
          .prefix = { 1, 0, 1, 0, 0, 0, 0 },
          .cap = MaxSignatureValues },
        { .description = "callable exports",
          .kind = PduKind::Instantiated,
          .prefix = { 1, 0 },
          .cap = MaxExports },
        { .description = "host functions",
          .kind = PduKind::CreateRuntime,
          .prefix = { 1, 2, 3, 4 },
          .cap = MaxHostFunctions },
        { .description = "call values", .kind = PduKind::Call, .prefix = { 1, 2 }, .cap = MaxCallValues },
        { .description = "host call arguments",
          .kind = PduKind::CallHost,
          .prefix = { 1 },
          .cap = MaxHostParameters },
        { .description = "call results", .kind = PduKind::CallResult, .prefix = { 0 }, .cap = MaxCallValues },
    };
    for (auto const& [description, kind, prefix, cap]: cases)
    {
        INFO(description);
        auto const& row = PduTable.at(std::to_underlying(kind) - 1);
        auto const bodyWith = [&prefix](uint64_t count) {
            auto writer = Writer {};
            for (auto const value: prefix)
                writer.varint(value);
            writer.varint(count);
            return writer.take();
        };
        CHECK(errorOf(frameOf(std::to_underlying(kind), bodyWith(cap + 1)), row) == DecodeError::OverCap);
        CHECK(errorOf(frameOf(std::to_underlying(kind), bodyWith(cap)), row) == DecodeError::Truncated);
    }
}

TEST_CASE("PduCodec.directionAndChannelAreChecked", "[vtextension]")
{
    // Every kind decodes on each channel its row names, travelling its way, and on no other.
    for (auto const& sample: samples())
    {
        auto const& row = rowOf(sample);
        INFO("kind " << static_cast<int>(row.kind));
        auto const frame = encoded(sample);
        auto const otherWay =
            row.direction == Direction::ToHelper ? Direction::ToParent : Direction::ToHelper;
        for (auto const channel: { ChannelKind::Control, ChannelKind::Lane })
        {
            INFO("channel " << static_cast<int>(channel));
            auto const decoded = decodeFrame(frame, row.direction, channel, PduLimits {});
            if (travelsOn(row, channel))
                CHECK(decoded.has_value());
            else
                CHECK(decoded == std::unexpected(DecodeError::WrongChannel));
            CHECK(decodeFrame(frame, otherWay, channel, PduLimits {})
                  == std::unexpected(DecodeError::WrongDirection));
        }
    }

    // Memory grows during a lane's call and during instantiation, on the control channel: both ways, Grow up
    // and Grown down, travel either channel. Nothing else travels both.
    for (auto const kind: { PduKind::Grow, PduKind::Grown })
        CHECK(PduTable.at(std::to_underlying(kind) - 1).channels == ChannelSet::Either);
    CHECK(std::ranges::count(PduTable, ChannelSet::Either, &PduRow::channels) == 2);
    CHECK(PduTable.at(std::to_underlying(PduKind::Grow) - 1).direction == Direction::ToParent);
    CHECK(PduTable.at(std::to_underlying(PduKind::Grown) - 1).direction == Direction::ToHelper);
}

TEST_CASE("PduCodec.protocolVersionIsThree", "[vtextension]")
{
    STATIC_CHECK(ProtocolVersion == 3);
}

TEST_CASE("PduCodec.grantAtItsLargestFitsItsCap", "[vtextension]")
{
    // Every field at its largest: still under the Instantiate row's cap, and round-tripped.
    auto const largest = Pdu { Instantiate {
        .moduleId = UINT32_MAX,
        .instanceId = UINT32_MAX,
        .interruptSlot = UINT32_MAX,
        .memory =
            MemoryGrant { .handle = UINT64_MAX, .objectBytes = UINT64_MAX, .initialBytes = UINT64_MAX } } };
    auto const frame = encoded(largest);
    auto const& row = rowOf(largest);
    CHECK(bodyBytesOf(frame) <= row.sizeCap(PduLimits {}));
    auto const decoded = decodeFrame(frame, row.direction, ChannelKind::Control, PduLimits {});
    REQUIRE(decoded.has_value());
    CHECK(decoded->pdu == largest);

    // And so is a Grow.
    auto const grow = Pdu { Grow { .instanceId = UINT32_MAX, .bytes = UINT64_MAX } };
    CHECK(bodyBytesOf(encoded(grow)) <= rowOf(grow).sizeCap(PduLimits {}));
}

TEST_CASE("PduCodec.memoryObjectBytes", "[vtextension]")
{
    constexpr auto KiB = uint64_t { 1024 };
    constexpr auto MiB = 1024 * KiB;
    STATIC_CHECK(WasmPageBytes == 64 * KiB);
    STATIC_CHECK(memoryObjectBytes(0) == 64 * KiB);
    STATIC_CHECK(memoryObjectBytes(1) == 64 * KiB);
    STATIC_CHECK(memoryObjectBytes(64 * KiB) == 64 * KiB);
    STATIC_CHECK(memoryObjectBytes((64 * KiB) + 1) == 128 * KiB);
    STATIC_CHECK(memoryObjectBytes(32 * MiB) == 32 * MiB);
    STATIC_CHECK(memoryObjectBytes((32 * MiB) + 1) == (32 * MiB) + (64 * KiB));
}

TEST_CASE("PduCodec.mutatedFramesNeverCrash", "[vtextension]")
{
    auto const all = samples();
    auto const frames =
        std::ranges::to<std::vector<std::vector<uint8_t>>>(all | std::views::transform(encoded));
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
            auto const decoded = decodeFrame(frame, row.direction, homeChannelOf(row), PduLimits {});
            if (!decoded)
                continue;
            // What decodes is a frame of its own: it encodes again to what decodes alike.
            ++decodes;
            INFO("iteration " << iteration);
            CHECK(decoded->consumed <= frame.size());
            auto const redecoded =
                decodeFrame(encoded(decoded->pdu), row.direction, homeChannelOf(row), PduLimits {});
            REQUIRE(redecoded.has_value());
            CHECK(redecoded->pdu == decoded->pdu);
        }
        // Byte flips in a field's value leave many frames valid; a run that decoded none tested nothing.
        CHECK(decodes > 0);
    }
}
