// SPDX-License-Identifier: Apache-2.0
//
// The Component Model's Canonical ABI, for the types a WIT world is made of.
//
// One generic codec lifts and lowers every type: how a type is laid out in a guest's linear memory,
// and how it flattens into core-wasm parameters, is computed from a description of the type. This
// header knows no world: a world's records, enums and flags are described by RecordFields,
// EnumCaseCount and FlagsMemberCount rows (the statusline world's are in StatuslineWorld.hpp), so
// adding a record to a world is adding a row, not writing a codec.
//
// Every address and length a guest provides is checked against its linear memory before a byte is
// read or written, so a guest cannot make the host touch memory outside its own: a lift that would is
// ExtensionError::Trapped, as the Canonical ABI traps.
//
// Lifting reads guest memory only through IGuestMemory::read(), which copies: each value is decoded from a
// copy taken once, never from the guest's memory itself. A guest's memory may be written while the host
// lifts (by another process that maps it, or by another thread of the guest's), and a value decoded from
// the memory itself could have one length when it is checked and another when it is used.
#pragma once

#include <core/Flags.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/Utf8.hpp>

namespace vtextension::abi
{

/// An offset into a guest's linear memory.
using Address = uint32_t;

/// What every codec operation yields: the value, or why the guest could not be spoken to.
template <typename T>
using Result = std::expected<T, ExtensionError>;

/// A guest's linear memory, as the codec reaches it.
class IGuestMemory
{
  public:
    IGuestMemory() = default;
    IGuestMemory(IGuestMemory const&) = delete;
    IGuestMemory(IGuestMemory&&) = delete;
    IGuestMemory& operator=(IGuestMemory const&) = delete;
    IGuestMemory& operator=(IGuestMemory&&) = delete;
    virtual ~IGuestMemory() = default;

    /// Copies guest memory, once: lifting reads through nothing else. The copy is the host's, so whatever
    /// the guest's memory holds afterwards, the bytes decoded are the bytes checked.
    /// @param address Where the copy starts.
    /// @param into Where it goes; its size is how many bytes are copied.
    /// @return Nothing once copied; `Trapped` if any byte lies outside the memory.
    [[nodiscard]] virtual Result<void> read(Address address, std::span<std::byte> into) = 0;

    /// Guest memory to write: for lowering, and for proving a range inside the memory. Lifting never reads
    /// through it (read() is for that). The span is invalidated by the next allocate(), which may grow the
    /// memory.
    /// @param address Where the span starts.
    /// @param size How many bytes it spans.
    /// @return The bytes at [@p address, @p address + @p size); `Trapped` if any lies outside the
    ///         memory.
    [[nodiscard]] virtual Result<std::span<std::byte>> writable(Address address, uint32_t size) = 0;

    /// Allocates guest memory through the guest's `cabi_realloc`.
    /// @param size How many bytes.
    /// @param alignment Their alignment, a power of two.
    /// @return The allocation's address, which lies in memory and is aligned; or why the guest could
    ///         not allocate.
    [[nodiscard]] virtual Result<Address> allocate(uint32_t size, uint32_t alignment) = 0;

    /// Accounts for host memory a lift is about to spend: the bytes of a string or list it copies
    /// out, and the host-side size of the elements of a list it builds. Charged before the
    /// allocation, so a forged length is refused before the host acts on it.
    /// @param bytes How many bytes the lift is about to allocate on the host.
    /// @return `Trapped` once the lift has spent more than the memory's budget allows.
    [[nodiscard]] virtual Result<void> chargeLift(uint64_t bytes) = 0;
};

/// Reads flat core-wasm values, in order, and fails rather than reads past their end.
class FlatReader
{
  public:
    /// @param values The flat values to read.
    explicit FlatReader(std::span<uint32_t const> values) noexcept: _values { values } {}

    /// @return The next value; `Trapped` if there is none.
    [[nodiscard]] Result<uint32_t> next() noexcept
    {
        if (_values.empty())
            return std::unexpected(ExtensionError::Trapped);
        auto const value = _values.front();
        _values = _values.subspan(1);
        return value;
    }

  private:
    std::span<uint32_t const> _values;
};

/// Flat core-wasm values as lowering produces them: every flat type of this world is an i32.
using FlatValues = std::vector<uint32_t>;

/// @return @p offset rounded up to a multiple of @p alignment, a power of two.
[[nodiscard]] constexpr uint32_t alignTo(uint32_t offset, uint32_t alignment) noexcept
{
    return (offset + alignment - 1) & ~(alignment - 1);
}

/// The codec of one type. Each specialization provides:
///
/// - `size` and `alignment`: the type's layout in linear memory;
/// - `flatCount`: how many core-wasm values it flattens to;
/// - `store(memory, address, value)` and `load(memory, address)`: lowering into and lifting out of
///   linear memory;
/// - `lowerFlat(memory, value, out)` and `liftFlat(memory, reader)`: the same for flat values.
///   Strings and lists flatten to an address and a length, so they still go through memory.
template <typename T>
struct Codec;

/// Describes a record: specialize it with `static constexpr auto Members = std::tuple { &T::a, ... };`,
/// the record's fields in the world's declaration order. A tuple is described the same way.
template <typename T>
struct RecordFields;

/// Describes an enum: the number of its cases, which are its enumerators 0 to count - 1.
template <typename E>
inline constexpr uint32_t EnumCaseCount = 0;

/// Marks a two-case enum that stands for a WIT `bool`, which lifts any non-zero byte as `true`.
template <typename E>
inline constexpr bool IsWireBool = false;

/// Describes a flags type: the number of its members, which are its bits 0 to count - 1.
template <typename E>
inline constexpr uint32_t FlagsMemberCount = 0;

namespace detail
{
    /// The type a pointer to data member points to.
    template <typename MemberPointer>
    struct MemberType;

    template <typename Class, typename Member>
    struct MemberType<Member Class::*>
    {
        using Type = Member;
    };

    /// Decodes @p bytes as a little-endian unsigned integer.
    template <typename U>
    [[nodiscard]] constexpr U decodeInteger(std::span<std::byte const, sizeof(U)> bytes) noexcept
    {
        auto value = U { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, sizeof(U)))
            value |= static_cast<U>(std::to_integer<U>(bytes[index]) << (8 * index));
        return value;
    }

    /// Reads @p memory's bytes at @p address, copied once, as a little-endian unsigned integer.
    template <typename U>
    [[nodiscard]] Result<U> readInteger(IGuestMemory& memory, Address address)
    {
        auto bytes = std::array<std::byte, sizeof(U)> {};
        return memory.read(address, bytes).transform([&] { return decodeInteger<U>(bytes); });
    }

    /// Writes @p value at @p address of @p memory as a little-endian unsigned integer.
    template <typename U>
    [[nodiscard]] Result<void> storeInteger(IGuestMemory& memory, Address address, U value)
    {
        auto const bytes = memory.writable(address, sizeof(U));
        if (!bytes)
            return std::unexpected(bytes.error());
        for (auto const index: std::views::iota(std::size_t { 0 }, sizeof(U)))
            (*bytes)[index] = static_cast<std::byte>((value >> (8 * index)) & 0xFFU);
        return {};
    }

    /// The unsigned integer a discriminant of @p count cases is stored in.
    template <std::size_t Count>
    using DiscriminantType = std::conditional_t<(Count <= 0x100), uint8_t, uint16_t>;

    /// Whether @p address + @p size fits the 32-bit address space.
    [[nodiscard]] constexpr bool fitsAddressSpace(uint64_t address, uint64_t size) noexcept
    {
        return address + size <= uint64_t { std::numeric_limits<uint32_t>::max() } + 1;
    }

    /// Proves [@p address, @p address + @p size) inside @p memory before a value of that size is
    /// stored or loaded there field by field, so that no field's address can wrap around.
    [[nodiscard]] inline Result<void> checkRange(IGuestMemory& memory, Address address, uint32_t size)
    {
        if (!fitsAddressSpace(address, size))
            return std::unexpected(ExtensionError::Trapped);
        return memory.writable(address, size).transform([](std::span<std::byte>) {});
    }

    /// Lowers a (pointer, length) pair: the shape strings and lists share.
    [[nodiscard]] inline Result<void> storePointerAndLength(IGuestMemory& memory,
                                                            Address address,
                                                            Address pointer,
                                                            uint32_t length)
    {
        return checkRange(memory, address, 8)
            .and_then([&] { return storeInteger<uint32_t>(memory, address, pointer); })
            .and_then([&] { return storeInteger<uint32_t>(memory, address + 4, length); });
    }

    /// The lifted (pointer, length) pair of a string or list.
    struct PointerAndLength
    {
        Address pointer;
        uint32_t length;
    };

    /// Lifts a (pointer, length) pair from memory, both from one copy.
    [[nodiscard]] inline Result<PointerAndLength> loadPointerAndLength(IGuestMemory& memory, Address address)
    {
        if (!fitsAddressSpace(address, 8))
            return std::unexpected(ExtensionError::Trapped);
        auto bytes = std::array<std::byte, 8> {};
        return memory.read(address, bytes).transform([&] {
            auto const pair = std::span<std::byte const, 8> { bytes };
            return PointerAndLength { .pointer = decodeInteger<uint32_t>(pair.first<4>()),
                                      .length = decodeInteger<uint32_t>(pair.last<4>()) };
        });
    }

    /// Lifts a (pointer, length) pair from flat values.
    [[nodiscard]] inline Result<PointerAndLength> liftPointerAndLength(FlatReader& reader)
    {
        auto const pointer = reader.next();
        if (!pointer)
            return std::unexpected(pointer.error());
        return reader.next().transform(
            [&](uint32_t length) { return PointerAndLength { .pointer = *pointer, .length = length }; });
    }

    /// Copies @p bytes into a fresh guest allocation aligned to @p alignment.
    /// @return The allocation's address.
    [[nodiscard]] inline Result<Address> storeBytes(IGuestMemory& memory,
                                                    std::span<std::byte const> bytes,
                                                    uint32_t alignment)
    {
        if (bytes.size() > std::numeric_limits<uint32_t>::max())
            return std::unexpected(ExtensionError::OutOfMemory);
        auto const size = static_cast<uint32_t>(bytes.size());
        auto const address = memory.allocate(size, alignment);
        if (!address || size == 0)
            return address;
        return memory.writable(*address, size).transform([&](std::span<std::byte> target) {
            std::ranges::copy(bytes, target.begin());
            return *address;
        });
    }

    /// Proves the bytes a (pointer, length) pair of @p elementSize-byte elements spans inside @p memory,
    /// reading none of them.
    [[nodiscard]] inline Result<void> checkElements(IGuestMemory& memory,
                                                    PointerAndLength list,
                                                    uint32_t elementSize)
    {
        auto const bytes = uint64_t { list.length } * elementSize;
        if (!fitsAddressSpace(list.pointer, bytes))
            return std::unexpected(ExtensionError::Trapped);
        if (bytes == 0)
            return {};
        return checkRange(memory, list.pointer, static_cast<uint32_t>(bytes));
    }

    /// Copies the bytes a (pointer, length) pair of @p elementSize-byte elements spans, once, into a
    /// container of byte-sized values. What the container takes on the host is the caller's to have charged
    /// (IGuestMemory::chargeLift()) or bounded before.
    template <typename Container>
        requires(sizeof(typename Container::value_type) == 1)
    [[nodiscard]] Result<Container> readBytes(IGuestMemory& memory,
                                              PointerAndLength list,
                                              uint32_t elementSize)
    {
        auto const bytes = uint64_t { list.length } * elementSize;
        if (!fitsAddressSpace(list.pointer, bytes))
            return std::unexpected(ExtensionError::Trapped);
        auto container = Container(static_cast<std::size_t>(bytes), typename Container::value_type {});
        if (bytes == 0)
            return container;
        return memory.read(list.pointer, std::as_writable_bytes(std::span { container })).transform([&] {
            return std::move(container);
        });
    }
} // namespace detail

/// Unsigned and signed integers of 8 and 32 bits, and u64. A u64 travels only through memory in
/// this world, so it has no flat form.
template <typename I>
    requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
struct Codec<I>
{
    using Unsigned = std::make_unsigned_t<I>;
    static constexpr uint32_t Size = sizeof(I);
    static constexpr uint32_t Alignment = sizeof(I);
    static constexpr std::size_t FlatCount = 1;

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, I value)
    {
        return detail::storeInteger<Unsigned>(memory, address, static_cast<Unsigned>(value));
    }

    [[nodiscard]] static Result<I> load(IGuestMemory& memory, Address address)
    {
        return detail::readInteger<Unsigned>(memory, address).transform([](Unsigned value) {
            return static_cast<I>(value);
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& /*memory*/, I value, FlatValues& out)
        requires(sizeof(I) <= sizeof(uint32_t))
    {
        out.push_back(static_cast<uint32_t>(static_cast<Unsigned>(value)));
        return {};
    }

    [[nodiscard]] static Result<I> liftFlat(IGuestMemory& /*memory*/, FlatReader& reader)
        requires(sizeof(I) <= sizeof(uint32_t))
    {
        // A narrow integer arrives in an i32 whose upper bits the Canonical ABI ignores.
        return reader.next().transform(
            [](uint32_t value) { return static_cast<I>(static_cast<Unsigned>(value)); });
    }
};

/// A WIT `bool`: one byte, which lifts any non-zero value as `true`, as the Canonical ABI does.
template <>
struct Codec<bool>
{
    static constexpr uint32_t Size = 1;
    static constexpr uint32_t Alignment = 1;
    static constexpr std::size_t FlatCount = 1;

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, bool value)
    {
        return detail::storeInteger<uint8_t>(memory, address, value ? 1 : 0);
    }

    [[nodiscard]] static Result<bool> load(IGuestMemory& memory, Address address)
    {
        return detail::readInteger<uint8_t>(memory, address).transform([](uint8_t byte) {
            return byte != 0;
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& /*memory*/, bool value, FlatValues& out)
    {
        out.push_back(value ? 1 : 0);
        return {};
    }

    [[nodiscard]] static Result<bool> liftFlat(IGuestMemory& /*memory*/, FlatReader& reader)
    {
        return reader.next().transform([](uint32_t value) { return value != 0; });
    }
};

/// Enums, and the two-case enums that stand for a WIT `bool`: a discriminant of the smallest width.
template <typename E>
    requires(std::is_enum_v<E> && EnumCaseCount<E> > 0)
struct Codec<E>
{
    using Discriminant = detail::DiscriminantType<EnumCaseCount<E>>;
    static constexpr uint32_t Size = sizeof(Discriminant);
    static constexpr uint32_t Alignment = sizeof(Discriminant);
    static constexpr std::size_t FlatCount = 1;

    /// Lifts a case index; a `bool` is any value, an enum must name one of its cases.
    [[nodiscard]] static Result<E> fromIndex(uint32_t index) noexcept
    {
        if constexpr (IsWireBool<E>)
            return static_cast<E>(index != 0 ? 1 : 0);
        else if (index >= EnumCaseCount<E>)
            return std::unexpected(ExtensionError::Trapped);
        else
            return static_cast<E>(index);
    }

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, E value)
    {
        return detail::storeInteger<Discriminant>(memory, address, static_cast<Discriminant>(value));
    }

    [[nodiscard]] static Result<E> load(IGuestMemory& memory, Address address)
    {
        return detail::readInteger<Discriminant>(memory, address).and_then([](Discriminant index) {
            return fromIndex(index);
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& /*memory*/, E value, FlatValues& out)
    {
        out.push_back(static_cast<uint32_t>(value));
        return {};
    }

    [[nodiscard]] static Result<E> liftFlat(IGuestMemory& /*memory*/, FlatReader& reader)
    {
        return reader.next().and_then([](uint32_t index) { return fromIndex(index); });
    }
};

/// Flags: one bit per member, in the smallest integer that holds them. Bits beyond the members are
/// ignored when lifting and never lowered.
template <typename E>
    requires(FlagsMemberCount<E> > 0 && FlagsMemberCount<E> <= 32)
struct Codec<core::Flags<E>>
{
    using Bits = std::conditional_t<(FlagsMemberCount<E> <= 8),
                                    uint8_t,
                                    std::conditional_t<(FlagsMemberCount<E> <= 16), uint16_t, uint32_t>>;
    static constexpr uint32_t Size = sizeof(Bits);
    static constexpr uint32_t Alignment = sizeof(Bits);
    static constexpr std::size_t FlatCount = 1;
    static constexpr uint32_t Mask =
        FlagsMemberCount<E> == 32 ? std::numeric_limits<uint32_t>::max() : (1U << FlagsMemberCount<E>) -1;

    [[nodiscard]] static core::Flags<E> fromBits(uint32_t bits) noexcept
    {
        return core::Flags<E>::fromValue(static_cast<core::Flags<E>::value_type>(bits & Mask));
    }

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, core::Flags<E> value)
    {
        return detail::storeInteger<Bits>(memory, address, static_cast<Bits>(value.value() & Mask));
    }

    [[nodiscard]] static Result<core::Flags<E>> load(IGuestMemory& memory, Address address)
    {
        return detail::readInteger<Bits>(memory, address).transform([](Bits bits) { return fromBits(bits); });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& /*memory*/,
                                                core::Flags<E> value,
                                                FlatValues& out)
    {
        out.push_back(static_cast<uint32_t>(value.value()) & Mask);
        return {};
    }

    [[nodiscard]] static Result<core::Flags<E>> liftFlat(IGuestMemory& /*memory*/, FlatReader& reader)
    {
        return reader.next().transform([](uint32_t bits) { return fromBits(bits); });
    }
};

/// Wall-clock time, as a u64 count of milliseconds since the Unix epoch.
template <>
struct Codec<std::chrono::sys_time<std::chrono::milliseconds>>
{
    using Time = std::chrono::sys_time<std::chrono::milliseconds>;
    static constexpr uint32_t Size = Codec<uint64_t>::Size;
    static constexpr uint32_t Alignment = Codec<uint64_t>::Alignment;
    static constexpr std::size_t FlatCount = 1;

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, Time value)
    {
        return Codec<uint64_t>::store(
            memory, address, static_cast<uint64_t>(value.time_since_epoch().count()));
    }

    [[nodiscard]] static Result<Time> load(IGuestMemory& memory, Address address)
    {
        return Codec<uint64_t>::load(memory, address).transform([](uint64_t count) {
            return Time { std::chrono::milliseconds { static_cast<std::chrono::milliseconds::rep>(count) } };
        });
    }
};

/// An offset from UTC, as an s32 count of minutes. An offset outside s32 has no meaning; it is clamped.
template <>
struct Codec<std::chrono::minutes>
{
    static constexpr uint32_t Size = Codec<int32_t>::Size;
    static constexpr uint32_t Alignment = Codec<int32_t>::Alignment;
    static constexpr std::size_t FlatCount = 1;

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, std::chrono::minutes value)
    {
        using Limits = std::numeric_limits<int32_t>;
        auto const minutes =
            std::clamp<std::chrono::minutes::rep>(value.count(), Limits::min(), Limits::max());
        return Codec<int32_t>::store(memory, address, static_cast<int32_t>(minutes));
    }

    [[nodiscard]] static Result<std::chrono::minutes> load(IGuestMemory& memory, Address address)
    {
        return Codec<int32_t>::load(memory, address).transform([](int32_t minutes) {
            return std::chrono::minutes { minutes };
        });
    }
};

/// An f32: its IEEE 754 bits in memory, and in a flat value as the i32 of the same bits. Where a variant's
/// cases put an f32 and an i32 at the same flat position, the Canonical ABI joins them into that i32, and
/// this world's every f32 shares its position with an i32 of another case -- which StatuslineWorld.hpp
/// asserts, so that a world change passing an f32 flat anywhere else fails to compile here instead of being
/// mislowered.
template <>
struct Codec<float>
{
    static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
    static constexpr uint32_t Size = sizeof(float);
    static constexpr uint32_t Alignment = sizeof(float);
    static constexpr std::size_t FlatCount = 1;

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, float value)
    {
        return Codec<uint32_t>::store(memory, address, std::bit_cast<uint32_t>(value));
    }

    [[nodiscard]] static Result<float> load(IGuestMemory& memory, Address address)
    {
        return Codec<uint32_t>::load(memory, address).transform([](uint32_t bits) {
            return std::bit_cast<float>(bits);
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& /*memory*/, float value, FlatValues& out)
    {
        out.push_back(std::bit_cast<uint32_t>(value));
        return {};
    }

    [[nodiscard]] static Result<float> liftFlat(IGuestMemory& /*memory*/, FlatReader& reader)
    {
        return reader.next().transform([](uint32_t bits) { return std::bit_cast<float>(bits); });
    }
};

/// Strings: UTF-8 bytes in a guest allocation, as (pointer, length). Lifting refuses invalid UTF-8.
template <>
struct Codec<std::string>
{
    static constexpr uint32_t Size = 8;
    static constexpr uint32_t Alignment = 4;
    static constexpr std::size_t FlatCount = 2;

    /// Copies @p value into the guest.
    [[nodiscard]] static Result<detail::PointerAndLength> lowerContents(IGuestMemory& memory,
                                                                        std::string const& value)
    {
        return detail::storeBytes(memory, std::as_bytes(std::span { value }), 1)
            .transform([&](Address pointer) {
                return detail::PointerAndLength { .pointer = pointer,
                                                  .length = static_cast<uint32_t>(value.size()) };
            });
    }

    /// Copies the guest's string at @p list out, checking that it is UTF-8.
    [[nodiscard]] static Result<std::string> liftContents(IGuestMemory& memory, detail::PointerAndLength list)
    {
        return memory.chargeLift(list.length)
            .and_then([&] { return detail::readBytes<std::string>(memory, list, 1); })
            .and_then([](std::string text) -> Result<std::string> {
                if (!isValidUtf8(text))
                    return std::unexpected(ExtensionError::Trapped);
                return text;
            });
    }

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, std::string const& value)
    {
        return lowerContents(memory, value).and_then([&](detail::PointerAndLength list) {
            return detail::storePointerAndLength(memory, address, list.pointer, list.length);
        });
    }

    [[nodiscard]] static Result<std::string> load(IGuestMemory& memory, Address address)
    {
        return detail::loadPointerAndLength(memory, address).and_then([&](detail::PointerAndLength list) {
            return liftContents(memory, list);
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& memory,
                                                std::string const& value,
                                                FlatValues& out)
    {
        return lowerContents(memory, value).transform([&](detail::PointerAndLength list) {
            out.push_back(list.pointer);
            out.push_back(list.length);
        });
    }

    [[nodiscard]] static Result<std::string> liftFlat(IGuestMemory& memory, FlatReader& reader)
    {
        return detail::liftPointerAndLength(reader).and_then(
            [&](detail::PointerAndLength list) { return liftContents(memory, list); });
    }
};

/// Lists: the elements laid out back to back in a guest allocation, as (pointer, length). A list of
/// bytes is copied in one go.
template <typename T>
struct Codec<std::vector<T>>
{
    static constexpr uint32_t Size = 8;
    static constexpr uint32_t Alignment = 4;
    static constexpr std::size_t FlatCount = 2;
    static constexpr bool IsByteList = std::is_same_v<T, uint8_t>;

    /// Copies @p value into the guest.
    [[nodiscard]] static Result<detail::PointerAndLength> lowerContents(IGuestMemory& memory,
                                                                        std::vector<T> const& value)
    {
        if constexpr (IsByteList)
        {
            return detail::storeBytes(memory, std::as_bytes(std::span { value }), 1)
                .transform([&](Address pointer) {
                    return detail::PointerAndLength { .pointer = pointer,
                                                      .length = static_cast<uint32_t>(value.size()) };
                });
        }
        else
        {
            auto const bytes = uint64_t { value.size() } * Codec<T>::Size;
            if (bytes > std::numeric_limits<uint32_t>::max())
                return std::unexpected(ExtensionError::OutOfMemory);
            auto const pointer = memory.allocate(static_cast<uint32_t>(bytes), Codec<T>::Alignment);
            if (!pointer)
                return std::unexpected(pointer.error());
            auto elementAddress = *pointer;
            for (auto const& element: value)
            {
                if (auto stored = Codec<T>::store(memory, elementAddress, element); !stored)
                    return std::unexpected(stored.error());
                elementAddress += Codec<T>::Size;
            }
            return detail::PointerAndLength { .pointer = *pointer,
                                              .length = static_cast<uint32_t>(value.size()) };
        }
    }

    /// Copies the guest's list at @p list out.
    [[nodiscard]] static Result<std::vector<T>> liftContents(IGuestMemory& memory,
                                                             detail::PointerAndLength list)
    {
        // Proves the whole list in bounds before reading any of it, and bounds the reservation below. The
        // length is the one lifted with the pointer, once: every element read below lies in the range proven.
        if (auto const inRange = detail::checkElements(memory, list, Codec<T>::Size); !inRange)
            return std::unexpected(inRange.error());
        if (auto charged = memory.chargeLift(uint64_t { list.length } * sizeof(T)); !charged)
            return std::unexpected(charged.error());
        if constexpr (IsByteList)
            return detail::readBytes<std::vector<uint8_t>>(memory, list, 1);
        else
        {
            auto elements = std::vector<T> {};
            elements.reserve(list.length);
            for (auto const index: std::views::iota(uint32_t { 0 }, list.length))
            {
                auto element = Codec<T>::load(memory, list.pointer + (index * Codec<T>::Size));
                if (!element)
                    return std::unexpected(element.error());
                elements.push_back(std::move(*element));
            }
            return elements;
        }
    }

    [[nodiscard]] static Result<void> store(IGuestMemory& memory,
                                            Address address,
                                            std::vector<T> const& value)
    {
        return lowerContents(memory, value).and_then([&](detail::PointerAndLength list) {
            return detail::storePointerAndLength(memory, address, list.pointer, list.length);
        });
    }

    [[nodiscard]] static Result<std::vector<T>> load(IGuestMemory& memory, Address address)
    {
        return detail::loadPointerAndLength(memory, address).and_then([&](detail::PointerAndLength list) {
            return liftContents(memory, list);
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& memory,
                                                std::vector<T> const& value,
                                                FlatValues& out)
    {
        return lowerContents(memory, value).transform([&](detail::PointerAndLength list) {
            out.push_back(list.pointer);
            out.push_back(list.length);
        });
    }

    [[nodiscard]] static Result<std::vector<T>> liftFlat(IGuestMemory& memory, FlatReader& reader)
    {
        return detail::liftPointerAndLength(reader).and_then(
            [&](detail::PointerAndLength list) { return liftContents(memory, list); });
    }
};

namespace detail
{
    /// How a record codec reaches the fields RecordFields names: through member pointers.
    template <typename T>
    struct MemberPointerAccess
    {
        static constexpr auto Members = RecordFields<T>::Members;
        static constexpr std::size_t FieldCount = std::tuple_size_v<decltype(Members)>;

        // Named through the member pointer's type rather than a decltype of `value.*member`: GCC 15 crashes
        // (internal compiler error in tsubst) on the latter inside the generic lambdas of RecordCodec.
        template <std::size_t I>
        using Field = std::remove_cvref_t<typename MemberType<std::remove_cvref_t<
            std::tuple_element_t<I, std::remove_cvref_t<decltype(RecordFields<T>::Members)>>>>::Type>;

        template <std::size_t I>
        [[nodiscard]] static constexpr Field<I> const& get(T const& value) noexcept
        {
            return value.*std::get<I>(Members);
        }

        template <std::size_t I>
        [[nodiscard]] static constexpr Field<I>& get(T& value) noexcept
        {
            return value.*std::get<I>(Members);
        }
    };

    /// The codec of a record, whatever describes its fields: each field at its natural alignment, in order.
    ///
    /// @tparam T The record.
    /// @tparam Access How its fields are reached: `FieldCount`, `Field<I>`, and `get<I>(value)` for a
    ///         const and a mutable record. RecordFields rows go through MemberPointerAccess; a world may
    ///         describe its records another way (by reflection, say) and share this codec.
    template <typename T, typename Access>
    struct RecordCodec
    {
        static constexpr std::size_t FieldCount = Access::FieldCount;

        template <std::size_t I>
        using Field = Access::template Field<I>;

        /// Each field's offset from the record's start.
        static constexpr auto Offsets = []<std::size_t... I>(std::index_sequence<I...>) {
            auto result = std::array<uint32_t, FieldCount> {};
            auto offset = uint32_t { 0 };
            ((offset = alignTo(offset, Codec<Field<I>>::Alignment),
              result[I] = offset,
              offset += Codec<Field<I>>::Size),
             ...);
            return result;
        }(std::make_index_sequence<FieldCount> {});

        static constexpr uint32_t Alignment = []<std::size_t... I>(std::index_sequence<I...>) {
            return std::max({ uint32_t { 1 }, Codec<Field<I>>::Alignment... });
        }(std::make_index_sequence<FieldCount> {});

        // The end of the last field, rounded up to the record's alignment. Folded over every field rather
        // than naming the last one: MSVC instantiates Field<FieldCount - 1> even in a discarded branch, which
        // a record without fields cannot satisfy.
        static constexpr uint32_t Size = []<std::size_t... I>(std::index_sequence<I...>) {
            auto end = uint32_t { 0 };
            ((end = Offsets[I] + Codec<Field<I>>::Size), ...);
            return alignTo(end, Alignment);
        }(std::make_index_sequence<FieldCount> {});

        static constexpr std::size_t FlatCount = []<std::size_t... I>(std::index_sequence<I...>) {
            return (std::size_t { 0 } + ... + Codec<Field<I>>::FlatCount);
        }(std::make_index_sequence<FieldCount> {});

        [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, T const& value)
        {
            auto result = checkRange(memory, address, Size);
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((result = result.and_then([&] {
                     return Codec<Field<I>>::store(
                         memory, address + Offsets[I], Access::template get<I>(value));
                 })),
                 ...);
            }(std::make_index_sequence<FieldCount> {});
            return result;
        }

        [[nodiscard]] static Result<T> load(IGuestMemory& memory, Address address)
        {
            auto value = T {};
            auto result = checkRange(memory, address, Size);
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((result = result.and_then([&] {
                     return Codec<Field<I>>::load(memory, address + Offsets[I])
                         .transform(
                             [&](Field<I> field) { Access::template get<I>(value) = std::move(field); });
                 })),
                 ...);
            }(std::make_index_sequence<FieldCount> {});
            return result.transform([&] { return std::move(value); });
        }

        [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& memory, T const& value, FlatValues& out)
        {
            auto result = Result<void> {};
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((result = result.and_then([&] {
                     return Codec<Field<I>>::lowerFlat(memory, Access::template get<I>(value), out);
                 })),
                 ...);
            }(std::make_index_sequence<FieldCount> {});
            return result;
        }

        [[nodiscard]] static Result<T> liftFlat(IGuestMemory& memory, FlatReader& reader)
        {
            auto value = T {};
            auto result = Result<void> {};
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((result = result.and_then([&] {
                     return Codec<Field<I>>::liftFlat(memory, reader).transform([&](Field<I> field) {
                         Access::template get<I>(value) = std::move(field);
                     });
                 })),
                 ...);
            }(std::make_index_sequence<FieldCount> {});
            return result.transform([&] { return std::move(value); });
        }
    };
} // namespace detail

/// Records and tuples: the fields RecordFields names, each at its natural alignment, in order.
template <typename T>
    requires requires { RecordFields<T>::Members; }
struct Codec<T>: detail::RecordCodec<T, detail::MemberPointerAccess<T>>
{
};

/// Variants: a discriminant of the smallest width, then the case's payload at the alignment of the
/// widest one. Flattened, the discriminant comes first and every case's payload fills the same
/// slots, padded with zeros to the widest case; all of this world's flat values are i32, so no
/// slot needs widening.
template <typename... Ts>
struct Codec<std::variant<Ts...>>
{
    using Variant = std::variant<Ts...>;
    using Discriminant = detail::DiscriminantType<sizeof...(Ts)>;

    static constexpr uint32_t PayloadAlignment = std::max({ uint32_t { 1 }, Codec<Ts>::Alignment... });
    static constexpr uint32_t Alignment = std::max(uint32_t { sizeof(Discriminant) }, PayloadAlignment);
    static constexpr uint32_t PayloadOffset = alignTo(sizeof(Discriminant), PayloadAlignment);
    static constexpr uint32_t Size =
        alignTo(PayloadOffset + std::max({ uint32_t { 0 }, Codec<Ts>::Size... }), Alignment);
    static constexpr std::size_t PayloadFlatCount = std::max({ std::size_t { 0 }, Codec<Ts>::FlatCount... });
    static constexpr std::size_t FlatCount = 1 + PayloadFlatCount;

    /// Constructs case @p I from what @p lift reads.
    template <std::size_t I, typename Lift>
    [[nodiscard]] static Result<Variant> liftCase(Lift const& lift)
    {
        using Case = std::variant_alternative_t<I, Variant>;
        return lift(std::type_identity<Case> {}).transform([](Case payload) {
            return Variant { std::in_place_index<I>, std::move(payload) };
        });
    }

    /// Constructs the case @p index names from what @p lift reads; `Trapped` for an unknown case.
    template <typename Lift>
    [[nodiscard]] static Result<Variant> liftIndexed(uint32_t index, Lift const& lift)
    {
        if (index >= sizeof...(Ts))
            return std::unexpected(ExtensionError::Trapped);
        static constexpr auto Cases = []<std::size_t... I>(std::index_sequence<I...>) {
            return std::array { &liftCase<I, Lift>... };
        }(std::index_sequence_for<Ts...> {});
        return Cases.at(index)(lift);
    }

    [[nodiscard]] static Result<void> store(IGuestMemory& memory, Address address, Variant const& value)
    {
        if (auto const inRange = detail::checkRange(memory, address, Size); !inRange)
            return std::unexpected(inRange.error());
        return detail::storeInteger<Discriminant>(memory, address, static_cast<Discriminant>(value.index()))
            .and_then([&] {
                return std::visit(
                    [&]<typename Case>(Case const& payload) {
                        return Codec<Case>::store(memory, address + PayloadOffset, payload);
                    },
                    value);
            });
    }

    [[nodiscard]] static Result<Variant> load(IGuestMemory& memory, Address address)
    {
        if (auto const inRange = detail::checkRange(memory, address, Size); !inRange)
            return std::unexpected(inRange.error());
        return detail::readInteger<Discriminant>(memory, address).and_then([&](Discriminant index) {
            return liftIndexed(index, [&]<typename Case>(std::type_identity<Case>) {
                return Codec<Case>::load(memory, address + PayloadOffset);
            });
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& memory, Variant const& value, FlatValues& out)
    {
        out.push_back(static_cast<uint32_t>(value.index()));
        auto const payloadStart = out.size();
        return std::visit([&]<typename Case>(
                              Case const& payload) { return Codec<Case>::lowerFlat(memory, payload, out); },
                          value)
            .transform([&] { out.resize(payloadStart + PayloadFlatCount, 0); });
    }

    [[nodiscard]] static Result<Variant> liftFlat(IGuestMemory& memory, FlatReader& reader)
    {
        auto const index = reader.next();
        if (!index)
            return std::unexpected(index.error());
        auto payload = std::array<uint32_t, PayloadFlatCount> {};
        for (auto& slot: payload)
        {
            auto const next = reader.next();
            if (!next)
                return std::unexpected(next.error());
            slot = *next;
        }
        return liftIndexed(*index, [&]<typename Case>(std::type_identity<Case>) {
            auto caseReader = FlatReader(payload);
            return Codec<Case>::liftFlat(memory, caseReader);
        });
    }
};

/// Results: a variant whose case 0 is the value and case 1 the error.
template <typename T, typename E>
struct Codec<std::expected<T, E>>
{
    using Variant = std::variant<T, E>;
    using VariantCodec = Codec<Variant>;
    static constexpr uint32_t Size = VariantCodec::Size;
    static constexpr uint32_t Alignment = VariantCodec::Alignment;
    static constexpr std::size_t FlatCount = VariantCodec::FlatCount;

    [[nodiscard]] static Variant toVariant(std::expected<T, E> const& value)
    {
        if (value)
            return Variant { std::in_place_index<0>, *value };
        return Variant { std::in_place_index<1>, value.error() };
    }

    [[nodiscard]] static std::expected<T, E> fromVariant(Variant value)
    {
        if (value.index() == 0)
            return std::move(std::get<0>(value));
        return std::unexpected(std::move(std::get<1>(value)));
    }

    [[nodiscard]] static Result<void> store(IGuestMemory& memory,
                                            Address address,
                                            std::expected<T, E> const& value)
    {
        return VariantCodec::store(memory, address, toVariant(value));
    }

    [[nodiscard]] static Result<std::expected<T, E>> load(IGuestMemory& memory, Address address)
    {
        return VariantCodec::load(memory, address).transform(&fromVariant);
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& memory,
                                                std::expected<T, E> const& value,
                                                FlatValues& out)
    {
        return VariantCodec::lowerFlat(memory, toVariant(value), out);
    }

    [[nodiscard]] static Result<std::expected<T, E>> liftFlat(IGuestMemory& memory, FlatReader& reader)
    {
        return VariantCodec::liftFlat(memory, reader).transform(&fromVariant);
    }
};

/// The `none` case of an `option`: a case without a payload.
template <>
struct RecordFields<std::monostate>
{
    static constexpr auto Members = std::tuple {};
};

/// Options: a variant whose case 0 is `none` and case 1 `some`.
template <typename T>
struct Codec<std::optional<T>>
{
    using Variant = std::variant<std::monostate, T>;
    using VariantCodec = Codec<Variant>;
    static constexpr uint32_t Size = VariantCodec::Size;
    static constexpr uint32_t Alignment = VariantCodec::Alignment;
    static constexpr std::size_t FlatCount = VariantCodec::FlatCount;

    [[nodiscard]] static Variant toVariant(std::optional<T> const& value)
    {
        if (value)
            return Variant { std::in_place_index<1>, *value };
        return Variant { std::in_place_index<0> };
    }

    [[nodiscard]] static std::optional<T> fromVariant(Variant value)
    {
        if (value.index() == 1)
            return std::move(std::get<1>(value));
        return std::nullopt;
    }

    [[nodiscard]] static Result<void> store(IGuestMemory& memory,
                                            Address address,
                                            std::optional<T> const& value)
    {
        return VariantCodec::store(memory, address, toVariant(value));
    }

    [[nodiscard]] static Result<std::optional<T>> load(IGuestMemory& memory, Address address)
    {
        return VariantCodec::load(memory, address).transform(&fromVariant);
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& memory,
                                                std::optional<T> const& value,
                                                FlatValues& out)
    {
        return VariantCodec::lowerFlat(memory, toVariant(value), out);
    }

    [[nodiscard]] static Result<std::optional<T>> liftFlat(IGuestMemory& memory, FlatReader& reader)
    {
        return VariantCodec::liftFlat(memory, reader).transform(&fromVariant);
    }
};

/// Lifts a `list<u8>` at @p address directly into a string, refusing one longer than @p maxLength
/// before copying any of it.
/// @param memory The guest's memory.
/// @param address Where the list's (pointer, length) pair is.
/// @param maxLength The most bytes the list may hold.
/// @return The bytes; `OutputTooLarge` if there are more than @p maxLength, `Trapped` if the pair or
///         the bytes lie outside the memory.
[[nodiscard]] Result<std::string> loadByteString(IGuestMemory& memory,
                                                 Address address,
                                                 std::size_t maxLength);

/// Lowers @p value into a fresh guest allocation of its own.
/// @return The allocation's address.
template <typename T>
[[nodiscard]] Result<Address> storeNew(IGuestMemory& memory, T const& value)
{
    return memory.allocate(Codec<T>::Size, Codec<T>::Alignment).and_then([&](Address address) {
        return Codec<T>::store(memory, address, value).transform([&] { return address; });
    });
}

/// Tuples of two: the pair's fields, in order.
template <typename A, typename B>
struct RecordFields<std::pair<A, B>>
{
    static constexpr auto Members = std::tuple { &std::pair<A, B>::first, &std::pair<A, B>::second };
};

} // namespace vtextension::abi
