// SPDX-License-Identifier: Apache-2.0
//
// Every PDU is a struct whose fields are listed once, by members(); the writers and readers below walk that
// list by field type. A new PDU is a struct, its members(), its table row (PduTable.hpp) and an instantiation
// at the bottom of this file.
#include <vtextension/wasm/remote/proto/Codec.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/wasm/remote/proto/PduTable.hpp>

namespace vtextension::wasm::remote::proto
{

namespace
{
    /// The outcome of reading one field.
    using Status = std::expected<void, DecodeError>;

    /// Whether @p T is @p U, or a const @p U.
    template <typename T, typename U>
    concept Of = std::same_as<std::remove_const_t<T>, U>;

    /// An enumeration the wire carries as one byte.
    template <typename T>
    concept WireEnum = std::is_enum_v<T> && sizeof(T) == 1;

    // Whether a byte names an enumerator, for every enumeration the wire carries; a byte that does not is
    // BadEnum. A switch without a default, so that the pedantic build's -Wswitch stops it compiling when an
    // enumerator is added and not named here; ExtensionError is checked against its description table, which
    // its own static_assert keeps complete.
    constexpr bool isEnumerator(ValueType value) noexcept
    {
        switch (value)
        {
            case ValueType::Other:
            case ValueType::I32:
            case ValueType::I64:
            case ValueType::F32:
            case ValueType::F64: return true;
        }
        return false;
    }
    constexpr bool isEnumerator(ExternKind value) noexcept
    {
        switch (value)
        {
            case ExternKind::Function:
            case ExternKind::Memory:
            case ExternKind::Table:
            case ExternKind::Global: return true;
        }
        return false;
    }
    constexpr bool isEnumerator(Linkage value) noexcept
    {
        switch (value)
        {
            case Linkage::Unsatisfied:
            case Linkage::Satisfied: return true;
        }
        return false;
    }
    constexpr bool isEnumerator(CallFailure value) noexcept
    {
        switch (value)
        {
            case CallFailure::Trap:
            case CallFailure::Interrupted:
            case CallFailure::MemoryGrowthRefused:
            case CallFailure::HostFailure: return true;
        }
        return false;
    }
    constexpr bool isEnumerator(ExtensionError value) noexcept
    {
        return std::to_underlying(value) < ExtensionErrorTexts.size();
    }
    constexpr bool isEnumerator(MemoryMode value) noexcept
    {
        switch (value)
        {
            case MemoryMode::Copy:
            case MemoryMode::Shared: return true;
        }
        return false;
    }
    constexpr bool isEnumerator(LockdownState value) noexcept
    {
        switch (value)
        {
            case LockdownState::Failed:
            case LockdownState::Unsandboxed:
            case LockdownState::LockedDown: return true;
        }
        return false;
    }
    constexpr bool isEnumerator(ReleaseTarget value) noexcept
    {
        switch (value)
        {
            case ReleaseTarget::Instance:
            case ReleaseTarget::Module:
            case ReleaseTarget::Runtime: return true;
        }
        return false;
    }
    constexpr bool isEnumerator(MemoryStatus value) noexcept
    {
        switch (value)
        {
            case MemoryStatus::OutOfBounds:
            case MemoryStatus::Ok: return true;
        }
        return false;
    }

    /// A vector field, with the most elements the wire lets it hold. Its count is checked before a single
    /// element is read: the byte caps bound the body, not what its elements cost once decoded, and a
    /// description costs a hundred times its smallest encoding.
    /// @tparam Vector The field's type: a std::vector, const or not.
    template <typename Vector>
    struct Capped
    {
        Vector& values;       ///< The field.
        std::size_t maxCount; ///< The most elements it may hold.
    };

    /// @return @p values, capped at @p maxCount elements.
    template <typename Vector>
    constexpr Capped<Vector> capped(Vector& values, std::size_t maxCount) noexcept
    {
        return Capped<Vector> { .values = values, .maxCount = maxCount };
    }

    /// @return The fields a members() lists: references to scalar fields, and Capped vector fields by value.
    template <typename... Fields>
    constexpr std::tuple<Fields...> fields(Fields&&... list) noexcept
    {
        return std::tuple<Fields...> { std::forward<Fields>(list)... };
    }

    // The fields of every struct the wire carries, in wire order. Each takes the struct const or not, so that
    // the writers and the readers walk the one list. A vector is listed Capped, and only so: no reader or
    // writer takes a plain std::vector of elements, so an uncapped one does not compile.
    constexpr auto members(Of<FunctionSignature> auto& value) noexcept
    {
        return fields(capped(value.parameters, MaxSignatureValues),
                      capped(value.results, MaxSignatureValues));
    }
    constexpr auto members(Of<ImportDescription> auto& value) noexcept
    {
        return fields(value.module, value.name, value.kind, value.signature, value.linkage);
    }
    constexpr auto members(Of<ExportDescription> auto& value) noexcept
    {
        return fields(value.name, value.kind, value.signature, value.initialPages);
    }
    constexpr auto members(Of<WireHostFunction> auto& value) noexcept
    {
        return fields(value.module, value.name, value.parameterCount);
    }
    constexpr auto members(Of<Hello> auto& value) noexcept
    {
        return fields(value.version, value.memoryMode, value.lockdown);
    }
    constexpr auto members(Of<CreateRuntime> auto& value) noexcept
    {
        return fields(
            value.runtimeId, value.memoryBytes, value.stackBytes, capped(value.functions, MaxHostFunctions));
    }
    constexpr auto members(Of<RuntimeCreated> auto& value) noexcept
    {
        return fields(value.runtimeId, value.error);
    }
    constexpr auto members(Of<Load> auto& value) noexcept
    {
        return fields(value.runtimeId, value.moduleId, value.bytes);
    }
    constexpr auto members(Of<Loaded> auto& value) noexcept
    {
        return fields(value.moduleId,
                      value.error,
                      capped(value.imports, MaxImports),
                      capped(value.exports, MaxExports));
    }
    constexpr auto members(Of<Instantiate> auto& value) noexcept
    {
        return fields(value.moduleId, value.instanceId);
    }
    constexpr auto members(Of<Instantiated> auto& value) noexcept
    {
        return fields(value.instanceId, value.error, capped(value.callableExports, MaxExports));
    }
    constexpr auto members(Of<Release> auto& value) noexcept
    {
        return fields(value.target, value.id);
    }
    constexpr auto members(Of<Call> auto& value) noexcept
    {
        return fields(value.instanceId, value.exportOrdinal, capped(value.argv, MaxCallValues));
    }
    constexpr auto members(Of<CallHost> auto& value) noexcept
    {
        return fields(value.functionIndex, capped(value.arguments, MaxHostParameters));
    }
    constexpr auto members(Of<HostResult> auto& value) noexcept
    {
        return fields(value.error);
    }
    constexpr auto members(Of<CallResult> auto& value) noexcept
    {
        return fields(value.failure, capped(value.argv, MaxCallValues));
    }
    constexpr auto members(Of<MemoryRead> auto& value) noexcept
    {
        return fields(value.instanceId, value.address, value.size);
    }
    constexpr auto members(Of<MemoryData> auto& value) noexcept
    {
        return fields(value.status, value.bytes);
    }
    constexpr auto members(Of<MemoryWrite> auto& value) noexcept
    {
        return fields(value.instanceId, value.address, value.bytes);
    }

    /// A struct whose fields members() lists.
    template <typename T>
    concept Described = requires(T& value) { members(value); };

    // Writers, declared before any is defined, so that each finds the others whatever it nests.
    void write(Writer& writer, uint32_t value);
    void write(Writer& writer, uint64_t value);
    template <WireEnum E>
    void write(Writer& writer, E value);
    template <WireEnum E>
    void write(Writer& writer, std::optional<E> const& value);
    void write(Writer& writer, std::string const& value);
    void write(Writer& writer, std::vector<std::byte> const& value);
    template <typename T>
    void write(Writer& writer, Capped<std::vector<T> const> const& field);
    template <Described T>
    void write(Writer& writer, T const& value);

    void write(Writer& writer, uint32_t value)
    {
        writer.varint(value);
    }

    void write(Writer& writer, uint64_t value)
    {
        writer.varint(value);
    }

    template <WireEnum E>
    void write(Writer& writer, E value)
    {
        writer.u8(std::to_underlying(value));
    }

    /// A status: 0 for none, else the enumerator's value + 1.
    template <WireEnum E>
    void write(Writer& writer, std::optional<E> const& value)
    {
        writer.u8(value ? static_cast<uint8_t>(std::to_underlying(*value) + 1) : uint8_t { 0 });
    }

    void write(Writer& writer, std::string const& value)
    {
        writer.varint(value.size());
        writer.bytes(std::as_bytes(std::span { value }));
    }

    void write(Writer& writer, std::vector<std::byte> const& value)
    {
        writer.varint(value.size());
        writer.bytes(value);
    }

    template <typename T>
    void write(Writer& writer, Capped<std::vector<T> const> const& field)
    {
        writer.varint(field.values.size());
        for (auto const& value: field.values)
            write(writer, value);
    }

    template <Described T>
    void write(Writer& writer, T const& value)
    {
        std::apply([&writer](auto const&... list) { (write(writer, list), ...); }, members(value));
    }

    // Readers, likewise declared first.
    Status read(Reader& reader, uint32_t& value);
    Status read(Reader& reader, uint64_t& value);
    template <WireEnum E>
    Status read(Reader& reader, E& value);
    template <WireEnum E>
    Status read(Reader& reader, std::optional<E>& value);
    Status read(Reader& reader, std::string& value);
    Status read(Reader& reader, std::vector<std::byte>& value);
    template <typename T>
    Status read(Reader& reader, Capped<std::vector<T>> const& field);
    template <Described T>
    Status read(Reader& reader, T& value);

    Status read(Reader& reader, uint32_t& value)
    {
        return reader.varint32().transform([&value](uint32_t read) { value = read; });
    }

    Status read(Reader& reader, uint64_t& value)
    {
        return reader.varint64().transform([&value](uint64_t read) { value = read; });
    }

    template <WireEnum E>
    Status read(Reader& reader, E& value)
    {
        return reader.u8().and_then([&value](uint8_t byte) -> Status {
            auto const candidate = static_cast<E>(byte);
            if (!isEnumerator(candidate))
                return std::unexpected(DecodeError::BadEnum);
            value = candidate;
            return {};
        });
    }

    template <WireEnum E>
    Status read(Reader& reader, std::optional<E>& value)
    {
        return reader.u8().and_then([&value](uint8_t byte) -> Status {
            if (byte == 0)
            {
                value.reset();
                return {};
            }
            auto const candidate = static_cast<E>(byte - 1);
            if (!isEnumerator(candidate))
                return std::unexpected(DecodeError::BadEnum);
            value = candidate;
            return {};
        });
    }

    Status read(Reader& reader, std::string& value)
    {
        return reader.varint64()
            .and_then([&](uint64_t count) { return reader.bytes(count); })
            .transform(
                [&value](std::span<uint8_t const> bytes) { value.assign(bytes.begin(), bytes.end()); });
    }

    Status read(Reader& reader, std::vector<std::byte>& value)
    {
        return reader.varint64()
            .and_then([&](uint64_t count) { return reader.bytes(count); })
            .transform([&value](std::span<uint8_t const> bytes) {
                auto const view = std::as_bytes(bytes);
                value.assign(view.begin(), view.end());
            });
    }

    template <typename T>
    Status read(Reader& reader, Capped<std::vector<T>> const& field)
    {
        auto const count = reader.varint64();
        if (!count)
            return std::unexpected(count.error());
        if (*count > field.maxCount)
            return std::unexpected(DecodeError::OverCap);
        auto& values = field.values;
        values.clear();
        values.reserve(boundedReserveCount<T>(*count, reader.remaining()));
        for ([[maybe_unused]] auto const index: std::views::iota(uint64_t { 0 }, *count))
        {
            auto& element = values.emplace_back();
            if (auto const status = read(reader, element); !status)
                return status;
        }
        return {};
    }

    template <Described T>
    Status read(Reader& reader, T& value)
    {
        auto status = Status {};
        std::apply([&](auto&&... list) { static_cast<void>(((status = read(reader, list)) && ...)); },
                   members(value));
        return status;
    }

    /// @return The largest frame length any kind may declare under @p limits: its body and its kind.
    [[nodiscard]] uint64_t largestFrameLength(PduLimits const& limits) noexcept
    {
        return std::ranges::max(PduTable | std::views::transform([&limits](PduRow const& row) {
                                    return row.sizeCap(limits);
                                }))
               + MaxVarint32Bytes;
    }

} // namespace

template <typename Body>
std::expected<Pdu, DecodeError> decodeBody(std::span<uint8_t const> body)
{
    auto reader = Reader { body };
    auto value = Body {};
    return read(reader, value).and_then([&]() -> std::expected<Pdu, DecodeError> {
        if (!reader.atEnd())
            return std::unexpected(DecodeError::TrailingBytes);
        return Pdu { std::move(value) };
    });
}

PduKind kindOf(Pdu const& pdu) noexcept
{
    return static_cast<PduKind>(pdu.index() + 1);
}

std::vector<uint8_t> encodeFrame(Pdu const& pdu)
{
    // Every kind fits a one-byte varint, so the kind is its value's byte.
    static_assert(std::to_underlying(PduKind::MemoryWrite) < 0x80);
    auto body = Writer {};
    std::visit([&body](auto const& value) { write(body, value); }, pdu);
    auto const bodyBytes = body.take();

    auto frame = Writer {};
    frame.varint(1 + bodyBytes.size());
    frame.u8(std::to_underlying(kindOf(pdu)));
    frame.bytes(std::as_bytes(std::span { bodyBytes }));
    return frame.take();
}

std::expected<DecodedFrame, DecodeError> decodeFrame(std::span<uint8_t const> buffer,
                                                     Direction arriving,
                                                     ChannelKind channel,
                                                     PduLimits const& limits)
{
    auto header = Reader { buffer };
    auto const length = header.varint64();
    if (!length)
        return std::unexpected(length.error() == DecodeError::Truncated ? DecodeError::NeedMoreData
                                                                        : length.error());
    // Refused before the kind is known, so that no header makes the reader wait for more than any kind holds.
    if (*length > largestFrameLength(limits))
        return std::unexpected(DecodeError::OverCap);
    auto const frameStart = header.consumed();
    auto const available = buffer.size() - frameStart;
    auto const complete = available >= *length;

    // The kind lies inside the frame: running past what arrived waits, running past the frame is a lie.
    auto frame =
        Reader { buffer.subspan(frameStart, complete ? static_cast<std::size_t>(*length) : available) };
    auto const kind = frame.varint32();
    if (!kind)
        return std::unexpected(kind.error() == DecodeError::Truncated && !complete ? DecodeError::NeedMoreData
                                                                                   : kind.error());
    if (*kind == 0 || *kind > PduTable.size())
        return std::unexpected(DecodeError::UnknownKind);
    auto const& row = PduTable[*kind - 1];
    if (row.direction != arriving)
        return std::unexpected(DecodeError::WrongDirection);
    if (row.channel != channel)
        return std::unexpected(DecodeError::WrongChannel);
    auto const bodySize = *length - frame.consumed();
    if (bodySize > row.sizeCap(limits))
        return std::unexpected(DecodeError::OverCap);
    if (!complete)
        return std::unexpected(DecodeError::NeedMoreData);

    auto const consumed = frameStart + static_cast<std::size_t>(*length);
    return row.decode(buffer.subspan(frameStart + frame.consumed(), static_cast<std::size_t>(bodySize)))
        .transform(
            [consumed](Pdu&& pdu) { return DecodedFrame { .pdu = std::move(pdu), .consumed = consumed }; });
}

// The bodies the PduTable decodes, one per Pdu alternative; the table's static_assert names each.
template std::expected<Pdu, DecodeError> decodeBody<Hello>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<CreateRuntime>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<RuntimeCreated>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<Load>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<Loaded>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<Instantiate>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<Instantiated>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<Release>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<Call>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<CallHost>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<HostResult>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<CallResult>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<MemoryRead>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<MemoryData>(std::span<uint8_t const>);
template std::expected<Pdu, DecodeError> decodeBody<MemoryWrite>(std::span<uint8_t const>);

} // namespace vtextension::wasm::remote::proto
