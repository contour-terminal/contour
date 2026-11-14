// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Flags.hpp>
#include <core/codec/DecodeError.hpp>
#include <core/codec/Stream.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/// One list of fields per struct, read and written by the same code -- a tolerant wire format.
///
/// A struct names its fields once, in `visitFields(this auto& self, auto& visitor)`, calling
/// `visitor(id, self.field)` for each. The id is the field's name on the wire: stable, never reused, and
/// never renumbered, so a field added later is one more call with a fresh id. FieldEncoder and FieldDecoder
/// are the two visitors, so the encoder and the decoder cannot disagree about a field's type. Types that
/// cannot carry the member are named by a `visitForeignFields(visitor, value)` overload that
/// argument-dependent lookup finds through the codec's hooks tag (@see BasicFieldEncoder).
///
/// On the wire a struct is a run of fields, each `varint key, value` with `key = id << 1 | kind`:
///
///   - kind 0 (WireKind::Varint): the value is one varint -- bools, integers, floating point numbers, enums
///     (by their stable wire index, @see EnumWire), flag sets (by stable wire bits, @see FlagWire), durations
///     (zigzag nanoseconds) and wall-clock instants (zigzag nanoseconds since the Unix epoch);
///   - kind 1 (WireKind::Framed): the value is a length-prefixed payload -- strings, sequences, maps,
///     optionals, variants and nested structs.
///
/// A reader skips the fields it does not know, by their kind -- what a newer writer added -- and leaves a
/// field the stream lacks at its default -- what an older writer did not have yet. That is the whole of the
/// compatibility story: no row versions, no legacy decoders.
namespace core::codec
{

/// The stable wire table of an enumeration: `static constexpr std::array Values`, in wire order.
///
/// An enumerator crosses the wire as its index in its table, never as the in-memory ordinal, which a
/// reordering, an insertion or a renumbering of the enumeration would change under a stream already written.
/// A table is appended to, never reordered. An enumerator a table lacks cannot be encoded (it crosses as a
/// value every reader refuses), so a row forgotten is a refused stream, never a silently remapped one.
/// Specialize it (or partially specialize it, for a family of enumerations) in the project that owns the
/// enum.
template <typename Enum>
struct EnumWire;

/// An enumeration with a stable wire table.
template <typename Enum>
concept WireEnum = std::is_enum_v<Enum> && requires { EnumWire<Enum>::Values; };

/// The stable wire table of the flags of a flag set: `static constexpr std::array Values`, flag N crossing
/// as wire bit N. Appended to, never reordered.
template <typename Flag>
struct FlagWire;

template <typename T>
struct FlagSetTraits: std::false_type
{
};
template <typename Flag>
struct FlagSetTraits<Flags<Flag>>: std::true_type
{
    using FlagType = Flag;
};

/// A `core::Flags` set.
template <typename T>
concept FlagSetType = FlagSetTraits<T>::value;

/// The flag enumeration of the flag set @p FlagSet.
template <FlagSetType FlagSet>
using FlagOf = FlagSetTraits<FlagSet>::FlagType;

/// Narrows @p value to T.
/// @param value A decoded varint.
/// @return @p value as T, or InvalidValue when T cannot hold it.
template <typename T>
[[nodiscard]] std::expected<T, DecodeError> narrow(uint64_t value)
{
    if (value > static_cast<uint64_t>(std::numeric_limits<T>::max()))
        return std::unexpected { DecodeError::InvalidValue };
    return static_cast<T>(value);
}

/// Narrows a signed @p value to T.
/// @param value A decoded signed varint.
/// @return @p value as T, or InvalidValue when T cannot hold it.
template <typename T>
[[nodiscard]] std::expected<T, DecodeError> narrowSigned(int64_t value)
{
    if (value < static_cast<int64_t>(std::numeric_limits<T>::min())
        || value > static_cast<int64_t>(std::numeric_limits<T>::max()))
        return std::unexpected { DecodeError::InvalidValue };
    return static_cast<T>(value);
}

/// How a value is laid out on the wire. @see fieldKey
enum class WireKind : uint8_t
{
    Varint = 0, ///< One varint.
    Framed = 1, ///< A length-prefixed payload.
};

/// @return The key a field is written under.
[[nodiscard]] constexpr uint64_t fieldKey(uint32_t id, WireKind kind) noexcept
{
    return (uint64_t { id } << 1U) | static_cast<uint64_t>(kind);
}

/// Whether a scalar type's signedness or width differs between platforms, so its wire form would too: wchar_t
/// (signed 32 bit on x86 Linux, unsigned on AArch64 Linux, unsigned 16 bit on Windows) and long (32 bit on
/// Windows, 64 bit elsewhere). Where long is 64 bit it is the wire form of int64_t, so only the 32-bit long
/// of Windows is refused: that build breaks, instead of the data.
template <typename T>
inline constexpr bool IsPlatformDependentScalar =
    std::is_same_v<T, wchar_t>
    || ((std::is_same_v<T, long> || std::is_same_v<T, unsigned long>) && sizeof(T) != sizeof(int64_t));

/// What a decode may allocate beyond what the stream holds: every element of a sequence costs at least one
/// byte of the stream, but may cost much more memory, so the decode keeps an account of the memory it
/// builds and refuses a stream that asks for more than a fixed multiple of its own size.
class DecodeBudget
{
  public:
    /// How many bytes of memory a decode may build per byte of stream, on top of a fixed allowance.
    static constexpr size_t BytesPerStreamByte = 64;

    /// The fixed allowance: what a small stream may build regardless of its size.
    static constexpr size_t Allowance = size_t { 64 } * 1024 * 1024;

    /// @param streamSize The size of the stream being decoded.
    explicit DecodeBudget(size_t streamSize) noexcept:
        _left { streamSize > (std::numeric_limits<size_t>::max() - Allowance) / BytesPerStreamByte
                    ? std::numeric_limits<size_t>::max()
                    : (streamSize * BytesPerStreamByte) + Allowance }
    {
    }

    /// Accounts for @p bytes of memory the decode is about to build.
    /// @return Whether they fit what is left.
    [[nodiscard]] bool spend(size_t bytes) noexcept
    {
        if (bytes > _left)
            return false;
        _left -= bytes;
        return true;
    }

  private:
    size_t _left;
};

/// A boxed integer (`boxed::boxed<T, Tag>`).
template <typename T>
concept BoxedValue = requires(T value) {
    typename T::inner_type;
    value.value;
};

/// A type that lists its own fields.
template <typename Visitor, typename T>
concept ListsOwnFields = requires(T& value, Visitor& visitor) { value.visitFields(visitor); };

/// A map (`std::map`, `std::unordered_map`): written as its entries in ascending key order, so equal maps
/// have equal bytes whatever their iteration order.
template <typename T>
concept MapLike = requires {
    typename T::key_type;
    typename T::mapped_type;
};

template <typename T>
struct IsStdVector: std::false_type
{
};
template <typename T>
struct IsStdVector<std::vector<T>>: std::true_type
{
};
template <typename T>
struct IsStdOptional: std::false_type
{
};
template <typename T>
struct IsStdOptional<std::optional<T>>: std::true_type
{
};
template <typename T>
struct IsStdPair: std::false_type
{
};
template <typename A, typename B>
struct IsStdPair<std::pair<A, B>>: std::true_type
{
};
template <typename T>
struct IsStdArray: std::false_type
{
};
template <typename T, size_t N>
struct IsStdArray<std::array<T, N>>: std::true_type
{
};
template <typename T>
struct IsStdVariant: std::false_type
{
};
template <typename... Alternatives>
struct IsStdVariant<std::variant<Alternatives...>>: std::true_type
{
};
template <typename T>
struct IsWallClockInstant: std::false_type
{
};
template <typename Duration>
struct IsWallClockInstant<std::chrono::time_point<std::chrono::system_clock, Duration>>: std::true_type
{
};
template <typename T>
struct IsStdDuration: std::false_type
{
};
template <typename Rep, typename Period>
struct IsStdDuration<std::chrono::duration<Rep, Period>>: std::true_type
{
};

/// @return Whether a T crosses the wire as one varint.
template <typename T>
[[nodiscard]] consteval bool isVarintValue()
{
    if constexpr (std::is_same_v<T, bool> || std::is_enum_v<T> || std::is_integral_v<T>
                  || std::is_floating_point_v<T> || FlagSetType<T> || IsStdDuration<T>::value
                  || IsWallClockInstant<T>::value)
        return true;
    else if constexpr (BoxedValue<T>)
        return isVarintValue<typename T::inner_type>();
    else
        return false;
}

/// @return How a T crosses the wire.
template <typename T>
[[nodiscard]] consteval WireKind wireKindOf()
{
    return isVarintValue<T>() ? WireKind::Varint : WireKind::Framed;
}

/// The varint an enumerator crosses as that no table lists: never a valid index, so it is refused.
inline constexpr uint64_t UnlistedEnumerator = std::numeric_limits<uint64_t>::max();

/// @return Where @p value stands in its stable wire table, or UnlistedEnumerator.
template <WireEnum Enum>
[[nodiscard]] constexpr uint64_t wireIndexOf(Enum value) noexcept
{
    constexpr auto const& Values = EnumWire<Enum>::Values;
    for (auto const index: std::views::iota(size_t { 0 }, Values.size()))
        if (Values[index] == value)
            return index;
    return UnlistedEnumerator;
}

/// @return @p flags as wire bits, one per row of the flag's stable wire table; UnlistedEnumerator when a
///         flag is set that the table does not list.
template <FlagSetType Flags>
[[nodiscard]] constexpr uint64_t wireBitsOf(Flags flags) noexcept
{
    using Flag = FlagOf<Flags>;
    constexpr auto const& Values = FlagWire<Flag>::Values;
    auto bits = uint64_t { 0 };
    auto listed = typename Flags::value_type { 0 };
    for (auto const index: std::views::iota(size_t { 0 }, Values.size()))
    {
        listed |= static_cast<Flags::value_type>(Values[index]);
        if (flags.test(Values[index]))
            bits |= uint64_t { 1 } << index;
    }
    if ((flags.value() & ~listed) != 0)
        return UnlistedEnumerator;
    return bits;
}

/// @return The flag set @p bits name, or nullopt when a bit is set that the flag's wire table does not list.
template <FlagSetType Flags>
[[nodiscard]] constexpr std::optional<Flags> flagsFromWire(uint64_t bits) noexcept
{
    constexpr auto const& Values = FlagWire<FlagOf<Flags>>::Values;
    static_assert(Values.size() < std::numeric_limits<uint64_t>::digits, "a flag set crosses as one varint");
    if ((bits >> Values.size()) != 0)
        return std::nullopt;
    auto flags = Flags {};
    for (auto const index: std::views::iota(size_t { 0 }, Values.size()))
        if ((bits & (uint64_t { 1 } << index)) != 0)
            flags.enable(Values[index]);
    return flags;
}

/// The default hooks of a field codec: no foreign types beyond what the codec itself knows.
struct NoForeignFields
{
};

/// Writes the fields a struct lists.
///
/// @tparam Hooks A tag type naming the namespace that holds the `visitForeignFields(visitor, value)`
/// overloads
///         for types that cannot carry a `visitFields` member (a type of a library, say). They are found by
///         argument-dependent lookup through the tag, so a project's overloads live next to its tag, and two
///         projects' codecs never see each other's.
template <typename Hooks>
class BasicFieldEncoder
{
  public:
    /// @param out Where the fields go.
    explicit BasicFieldEncoder(Writer& out) noexcept: _out { out } {}

    /// Writes one field.
    /// @param id    The field's wire id.
    /// @param value The field.
    template <typename T>
    void operator()(uint32_t id, T const& value)
    {
        _out.varint(fieldKey(id, wireKindOf<T>()));
        writeValue(value);
    }

    /// Writes one field a list names for its readers; the name does not cross the wire.
    template <typename T>
    void operator()(uint32_t id, T const& value, std::string_view /*name*/)
    {
        (*this)(id, value);
    }

    /// Writes the fields of @p value without a frame around them: the top level of a stream.
    template <typename T>
    void writeFields(T const& value)
    {
        if constexpr (ListsOwnFields<BasicFieldEncoder, T const>)
            value.visitFields(*this);
        else
            visitForeignFields(*this, value);
    }

    /// Writes @p value as an element: a varint, or a length-prefixed payload.
    template <typename T>
    void writeValue(T const& value)
    {
        if constexpr (isVarintValue<T>())
            _out.varint(scalarOf(value));
        else if constexpr (std::is_same_v<T, std::string>)
            _out.string(value);
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
            _out.bytes(value); // Its length known up front: nothing to move once it is written.
        else
        {
            auto const frame = _out.beginFrame();
            writePayload(value);
            _out.endFrame(frame);
        }
    }

    /// @return The writer the fields go to.
    [[nodiscard]] Writer& writer() noexcept { return _out; }

  private:
    template <typename T>
    [[nodiscard]] static uint64_t scalarOf(T const& value)
    {
        constexpr auto Zigzag = [](int64_t signedValue) {
            return (static_cast<uint64_t>(signedValue) << 1U)
                   ^ static_cast<uint64_t>(signedValue >> std::numeric_limits<int64_t>::digits);
        };
        static_assert(!IsPlatformDependentScalar<T>,
                      "this scalar differs in signedness and width between platforms, so its wire form would "
                      "too: use a fixed-width type (int32_t, uint32_t, char32_t) for the field");
        if constexpr (std::is_same_v<T, bool>)
            return value ? 1 : 0;
        else if constexpr (std::is_same_v<T, char>)
            // The wire form is a signed byte, as on x86: char is unsigned on AArch64 Linux, which would
            // otherwise write the raw value and read the stream of another platform wrongly.
            return Zigzag(static_cast<int8_t>(value));
        else if constexpr (std::is_enum_v<T>)
        {
            static_assert(WireEnum<T>,
                          "an enum crosses the wire by its stable table: specialize EnumWire for it");
            return wireIndexOf(value);
        }
        else if constexpr (std::is_floating_point_v<T>)
            return std::bit_cast<uint64_t>(static_cast<double>(value));
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            return Zigzag(static_cast<int64_t>(value));
        else if constexpr (std::is_integral_v<T>)
            return static_cast<uint64_t>(value);
        else if constexpr (BoxedValue<T>)
            return scalarOf(value.value);
        else if constexpr (FlagSetType<T>)
            return wireBitsOf(value);
        else if constexpr (IsStdDuration<T>::value)
            return Zigzag(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
        else
            return Zigzag(
                std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
    }

    template <typename T>
    void writePayload(T const& value)
    {
        if constexpr (std::is_same_v<T, std::string>)
            _out.raw(std::span { reinterpret_cast<uint8_t const*>(value.data()), value.size() });
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
            _out.raw(value);
        else if constexpr (std::is_same_v<T, std::u32string>)
        {
            for (auto const codepoint: value)
                writeValue(codepoint);
        }
        else if constexpr (IsStdVector<T>::value || IsStdArray<T>::value)
        {
            for (auto const& element: value)
                writeValue(element);
        }
        else if constexpr (MapLike<T>)
        {
            auto entries = std::vector<typename T::value_type const*> {};
            entries.reserve(value.size());
            for (auto const& entry: value)
                entries.push_back(&entry);
            std::ranges::sort(entries, {}, [](auto const* entry) -> auto const& { return entry->first; });
            for (auto const* entry: entries)
            {
                writeValue(entry->first);
                writeValue(entry->second);
            }
        }
        else if constexpr (IsStdVariant<T>::value)
        {
            // The alternatives are numbered by their place in the variant, which is part of its declaration:
            // they are appended to, never reordered.
            _out.varint(value.index());
            std::visit([&](auto const& alternative) { writeValue(alternative); }, value);
        }
        else if constexpr (IsStdOptional<T>::value)
        {
            _out.u8(value.has_value() ? 1 : 0);
            if (value)
                writeValue(*value);
        }
        else if constexpr (IsStdPair<T>::value)
        {
            writeValue(value.first);
            writeValue(value.second);
        }
        else if constexpr (std::is_empty_v<T>)
        {
        }
        else
            writeFields(value);
    }

    Writer& _out;
};

/// Reads the fields a struct lists, out of one struct's payload. The first failure is kept and every
/// later read is skipped, so a row's decoder lists its fields and checks once.
///
/// @tparam Hooks The tag of BasicFieldEncoder, which see.
template <typename Hooks>
class BasicFieldDecoder
{
  public:
    /// Reads the keys of a struct's payload.
    /// @param payload The fields.
    /// @param budget  What the decode may still allocate; outlives the decoder.
    BasicFieldDecoder(std::span<uint8_t const> payload, DecodeBudget& budget): _budget { budget }
    {
        auto in = Reader { payload };
        while (!in.atEnd() && _status == Status::Reading)
            readKey(in);
    }

    /// Reads one field, if the payload carries it; leaves it as it is otherwise.
    /// @param id    The field's wire id.
    /// @param value Where it goes.
    template <typename T>
    void operator()(uint32_t id, T& value)
    {
        if (_status == Status::Failed)
            return;
        auto const field = std::ranges::find(_fields, id, &Field::id);
        if (field == _fields.end())
            return;
        if (field->kind != wireKindOf<T>())
        {
            fail(DecodeError::MalformedRecord);
            return;
        }
        if constexpr (isVarintValue<T>())
            readScalar(field->scalar, value);
        else
        {
            auto in = Reader { field->payload };
            readPayload(in, value);
        }
    }

    /// Reads one field a list names for its readers; the name does not cross the wire.
    template <typename T>
    void operator()(uint32_t id, T& value, std::string_view /*name*/)
    {
        (*this)(id, value);
    }

    /// Reads the fields of @p value from this decoder's payload.
    template <typename T>
    void readFields(T& value)
    {
        if constexpr (ListsOwnFields<BasicFieldDecoder, T>)
            value.visitFields(*this);
        else
            visitForeignFields(*this, value);
    }

    /// Records a failure found by the caller's own check, unless one is recorded already.
    /// @param error The failure.
    void fail(DecodeError error) noexcept
    {
        if (_status == Status::Reading)
        {
            _status = Status::Failed;
            _error = error;
        }
    }

    /// @return Nothing when every field was read, else the first failure.
    [[nodiscard]] std::expected<void, DecodeError> done() const
    {
        if (_status == Status::Failed)
            return std::unexpected { _error };
        return {};
    }

    /// @return What the decode may still allocate.
    [[nodiscard]] DecodeBudget& budget() noexcept { return _budget; }

  private:
    /// A field as it sits in the payload: its value, or a view of its payload.
    struct Field
    {
        uint32_t id = 0;
        WireKind kind = WireKind::Varint;
        uint64_t scalar = 0;
        std::span<uint8_t const> payload {};
    };

    void readKey(Reader& in)
    {
        auto const key = in.varint();
        if (!key)
        {
            fail(key.error());
            return;
        }
        auto const id = *key >> 1U;
        auto field = Field { .id = static_cast<uint32_t>(id),
                             .kind = (*key & 1U) != 0 ? WireKind::Framed : WireKind::Varint };
        if (id > std::numeric_limits<uint32_t>::max()
            || std::ranges::find(_fields, field.id, &Field::id) != _fields.end())
        {
            fail(DecodeError::MalformedRecord);
            return;
        }
        if (field.kind == WireKind::Varint)
        {
            if (auto const scalar = in.varint(); scalar)
                field.scalar = *scalar;
            else
                fail(scalar.error());
        }
        else if (auto const payload = in.bytes(); payload)
            field.payload = *payload;
        else
            fail(payload.error());
        _fields.push_back(field);
    }

    /// Reads one element: a varint or a length-prefixed payload.
    template <typename T>
    void readValue(Reader& in, T& value)
    {
        if (_status == Status::Failed)
            return;
        if constexpr (isVarintValue<T>())
        {
            if (auto const scalar = in.varint(); scalar)
                readScalar(*scalar, value);
            else
                fail(scalar.error());
        }
        else if (auto const payload = in.bytes(); payload)
        {
            auto inner = Reader { *payload };
            readPayload(inner, value);
        }
        else
            fail(payload.error());
    }

    template <typename T>
    void readScalar(uint64_t raw, T& value)
    {
        constexpr auto Unzigzag = [](uint64_t zigzag) {
            return static_cast<int64_t>(zigzag >> 1U) ^ -static_cast<int64_t>(zigzag & 1U);
        };
        static_assert(!IsPlatformDependentScalar<T>,
                      "this scalar differs in signedness and width between platforms, so its wire form would "
                      "too: use a fixed-width type (int32_t, uint32_t, char32_t) for the field");
        if constexpr (std::is_same_v<T, bool>)
        {
            if (raw > 1)
                fail(DecodeError::InvalidValue);
            else
                value = raw != 0;
        }
        else if constexpr (std::is_same_v<T, char>)
        {
            // A signed byte on the wire whatever char is here (see scalarOf).
            auto narrowed = int8_t {};
            take(narrowed, narrowSigned<int8_t>(Unzigzag(raw)));
            if (_status == Status::Reading)
                value = static_cast<char>(narrowed);
        }
        else if constexpr (std::is_enum_v<T>)
        {
            static_assert(WireEnum<T>,
                          "an enum crosses the wire by its stable table: specialize EnumWire for it");
            constexpr auto const& Values = EnumWire<T>::Values;
            if (raw >= Values.size())
                fail(DecodeError::InvalidValue);
            else
                value = Values[static_cast<size_t>(raw)];
        }
        else if constexpr (std::is_floating_point_v<T>)
            value = static_cast<T>(std::bit_cast<double>(raw));
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            take(value, narrowSigned<T>(Unzigzag(raw)));
        else if constexpr (std::is_integral_v<T>)
            take(value, narrow<T>(raw));
        else if constexpr (BoxedValue<T>)
        {
            auto inner = typename T::inner_type {};
            readScalar(raw, inner);
            if (_status == Status::Reading)
                value = T { inner };
        }
        else if constexpr (FlagSetType<T>)
            readFlags(raw, value);
        else if constexpr (IsStdDuration<T>::value)
            value = std::chrono::duration_cast<T>(std::chrono::nanoseconds { Unzigzag(raw) });
        else
            value = T { std::chrono::duration_cast<typename T::duration>(
                std::chrono::nanoseconds { Unzigzag(raw) }) };
    }

    template <typename Flags>
    void readFlags(uint64_t raw, Flags& value)
    {
        if (auto const flags = flagsFromWire<Flags>(raw))
            value = *flags;
        else
            fail(DecodeError::InvalidValue);
    }

    /// Reads @p value from the whole of @p in.
    template <typename T>
    void readPayload(Reader& in, T& value)
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            auto const data = in.raw(in.remaining());
            value.assign(reinterpret_cast<char const*>(data->data()), data->size());
        }
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
        {
            auto const data = in.raw(in.remaining());
            value.assign(data->begin(), data->end());
        }
        else if constexpr (std::is_same_v<T, std::u32string>)
        {
            value.clear();
            while (!in.atEnd() && _status == Status::Reading)
            {
                auto codepoint = char32_t {};
                readValue(in, codepoint);
                value.push_back(codepoint);
            }
        }
        else if constexpr (IsStdVector<T>::value)
        {
            value.clear();
            while (!in.atEnd() && _status == Status::Reading)
                if (spend(sizeof(typename T::value_type)))
                    readValue(in, value.emplace_back());
        }
        else if constexpr (IsStdArray<T>::value)
        {
            // A newer writer's longer array has its excess read and dropped; an older one's shorter array
            // leaves the rest at its default.
            auto index = size_t { 0 };
            while (!in.atEnd() && _status == Status::Reading)
            {
                auto element = typename T::value_type {};
                readValue(in, element);
                if (index < value.size())
                    value[index] = std::move(element);
                ++index;
            }
        }
        else if constexpr (MapLike<T>)
        {
            value.clear();
            while (!in.atEnd() && _status == Status::Reading)
            {
                auto key = typename T::key_type {};
                auto mapped = typename T::mapped_type {};
                readValue(in, key);
                readValue(in, mapped);
                if (_status == Status::Reading && spend(sizeof(typename T::value_type))
                    && !value.emplace(std::move(key), std::move(mapped)).second)
                    fail(DecodeError::MalformedRecord);
            }
        }
        else if constexpr (IsStdVariant<T>::value)
        {
            if (auto const index = in.varint(); index)
                emplaceAlternative(in, value, *index);
            else
                fail(index.error());
        }
        else if constexpr (IsStdOptional<T>::value)
        {
            auto const present = in.u8();
            if (!present)
                fail(present.error());
            else if (*present > 1)
                fail(DecodeError::InvalidValue);
            else if (*present == 0)
                value.reset();
            else
                readValue(in, value.emplace());
        }
        else if constexpr (IsStdPair<T>::value)
        {
            readValue(in, value.first);
            readValue(in, value.second);
        }
        else if constexpr (std::is_empty_v<T>)
        {
        }
        else
        {
            auto const payload = in.raw(in.remaining());
            auto nested = BasicFieldDecoder { *payload, _budget };
            nested.readFields(value);
            if (auto const done = nested.done(); !done)
                fail(done.error());
        }
        if (_status == Status::Reading && !in.atEnd())
            fail(DecodeError::MalformedRecord);
    }

    template <typename Variant, size_t Index = 0>
    void emplaceAlternative(Reader& in, Variant& value, uint64_t index)
    {
        if constexpr (Index < std::variant_size_v<Variant>)
        {
            if (index == Index)
                readValue(in, value.template emplace<Index>());
            else
                emplaceAlternative<Variant, Index + 1>(in, value, index);
        }
        else
            fail(DecodeError::InvalidValue);
    }

    /// Stores @p result into @p target, or records why it failed.
    template <typename Target, typename Result>
    void take(Target& target, Result&& result)
    {
        if (result)
            target = *std::forward<Result>(result);
        else
            fail(result.error());
    }

    [[nodiscard]] bool spend(size_t bytes)
    {
        if (_budget.spend(bytes))
            return true;
        fail(DecodeError::TooLarge);
        return false;
    }

    /// Whether a read has failed; once it has, _error says why.
    enum class Status : uint8_t
    {
        Reading,
        Failed,
    };

    DecodeBudget& _budget;
    std::vector<Field> _fields;
    // Two plain members, not one std::optional<DecodeError>: GCC 14 and 15 at -O3 report a disengaged
    // optional's payload (and an std::expected's error) as maybe-uninitialized once the decoder is inlined.
    Status _status = Status::Reading;
    DecodeError _error = DecodeError::Truncated;
};

/// Writes @p value's fields as a payload of their own, with no stream header.
/// @param writer The stream.
/// @tparam Hooks The foreign-types tag, @see BasicFieldEncoder.
/// @param value A struct that lists its fields.
template <typename Hooks = NoForeignFields, typename T>
void encodeFields(Writer& writer, T const& value)
{
    auto encoder = BasicFieldEncoder<Hooks> { writer };
    encoder.writeFields(value);
}

/// Reads @p value's fields from @p payload.
/// @param payload The fields, as encodeFields() wrote them.
/// @param value   Filled in; a field the payload lacks keeps the value it had.
/// @return Nothing, or why the payload is not a valid @p value.
template <typename Hooks = NoForeignFields, typename T>
[[nodiscard]] std::expected<void, DecodeError> decodeFields(std::span<uint8_t const> payload, T& value)
{
    auto budget = DecodeBudget { payload.size() };
    auto decoder = BasicFieldDecoder<Hooks> { payload, budget };
    decoder.readFields(value);
    return decoder.done();
}

/// The encoder for types that need no foreign-type hooks.
using FieldEncoder = BasicFieldEncoder<NoForeignFields>;

/// The decoder for types that need no foreign-type hooks.
using FieldDecoder = BasicFieldDecoder<NoForeignFields>;

} // namespace core::codec
