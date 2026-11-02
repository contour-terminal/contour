// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/GraphicsAttributes.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/state/StateCodec.hpp>
#include <vtbackend/state/StateValues.hpp>
#include <vtbackend/state/StateWire.hpp>

#include <crispy/Point.hpp>

#include <core/Flags.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/// One list of fields per state struct, read and written by the same code -- a tolerant wire format.
///
/// A state struct names its fields once, in `visitFields(this auto& self, auto& visitor)`, calling
/// `visitor(id, self.field)` for each. The id is the field's name on the wire: stable, never reused, and
/// never renumbered, so a field added later is one more call with a fresh id. FieldEncoder and FieldDecoder
/// are the two visitors, so the encoder and the decoder cannot disagree about a field's type. Foreign types
/// that cannot carry the member are named by a `visitStateFields(visitor, value)` overload below.
///
/// On the wire a struct is a run of fields, each `varint key, value` with `key = id << 1 | kind`:
///
///   - kind 0 (WireKind::Varint): the value is one varint -- bools, integers, enums (by their stable wire
///     index, @see StateEnumWire), flag sets (by stable wire bits, @see StateFlagWire), durations (zigzag
///     nanoseconds) and wall-clock instants (zigzag nanoseconds since the Unix epoch);
///   - kind 1 (WireKind::Framed): the value is a length-prefixed payload -- strings, sequences, maps,
///     optionals, variants and nested structs.
///
/// A reader skips the fields it does not know, by their kind -- what a newer writer added -- and leaves a
/// field the stream lacks at its default -- what an older writer did not have yet. That is the whole of the
/// compatibility story: no row versions, no legacy decoders.
namespace vtbackend::detail
{

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

/// A type some `visitStateFields` overload lists the fields of.
template <typename Visitor, typename T>
concept ListsForeignFields = requires(T& value, Visitor& visitor) { visitStateFields(visitor, value); };

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
    constexpr auto const& Values = StateEnumWire<Enum>::Values;
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
    constexpr auto const& Values = StateFlagWire<Flag>::Values;
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
    constexpr auto const& Values = StateFlagWire<FlagOf<Flags>>::Values;
    static_assert(Values.size() < std::numeric_limits<uint64_t>::digits, "a flag set crosses as one varint");
    if ((bits >> Values.size()) != 0)
        return std::nullopt;
    auto flags = Flags {};
    for (auto const index: std::views::iota(size_t { 0 }, Values.size()))
        if ((bits & (uint64_t { 1 } << index)) != 0)
            flags.enable(Values[index]);
    return flags;
}

/// Writes the fields a state struct lists.
class FieldEncoder
{
  public:
    /// @param out Where the fields go.
    explicit FieldEncoder(StateWriter& out) noexcept: _out { out } {}

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
        if constexpr (ListsOwnFields<FieldEncoder, T const>)
            value.visitFields(*this);
        else
            visitStateFields(*this, value);
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
    [[nodiscard]] StateWriter& writer() noexcept { return _out; }

  private:
    template <typename T>
    [[nodiscard]] static uint64_t scalarOf(T const& value)
    {
        constexpr auto Zigzag = [](int64_t signedValue) {
            return (static_cast<uint64_t>(signedValue) << 1U)
                   ^ static_cast<uint64_t>(signedValue >> std::numeric_limits<int64_t>::digits);
        };
        if constexpr (std::is_same_v<T, bool>)
            return value ? 1 : 0;
        else if constexpr (std::is_enum_v<T>)
        {
            static_assert(WireEnum<T>,
                          "an enum crosses the wire by its stable table: add it to StateWire.hpp");
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

    StateWriter& _out;
};

/// Reads the fields a state struct lists, out of one struct's payload. The first failure is kept and every
/// later read is skipped, so a row's decoder lists its fields and checks once.
class FieldDecoder
{
  public:
    /// Reads the keys of a struct's payload.
    /// @param payload The fields.
    /// @param budget  What the decode may still allocate; outlives the decoder.
    FieldDecoder(std::span<uint8_t const> payload, DecodeBudget& budget): _budget { budget }
    {
        auto in = StateReader::ofPayload(payload);
        while (!in.atEnd() && !_error)
            readKey(in);
    }

    /// Reads one field, if the payload carries it; leaves it as it is otherwise.
    /// @param id    The field's wire id.
    /// @param value Where it goes.
    template <typename T>
    void operator()(uint32_t id, T& value)
    {
        if (_error)
            return;
        auto const field = std::ranges::find(_fields, id, &Field::id);
        if (field == _fields.end())
            return;
        if (field->kind != wireKindOf<T>())
        {
            fail(StateDecodeError::MalformedRecord);
            return;
        }
        if constexpr (isVarintValue<T>())
            readScalar(field->scalar, value);
        else
        {
            auto in = StateReader::ofPayload(field->payload);
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
        if constexpr (ListsOwnFields<FieldDecoder, T>)
            value.visitFields(*this);
        else
            visitStateFields(*this, value);
    }

    /// Records a failure found by the caller's own check, unless one is recorded already.
    /// @param error The failure.
    void fail(StateDecodeError error) noexcept
    {
        if (!_error)
            _error = error;
    }

    /// @return Nothing when every field was read, else the first failure.
    [[nodiscard]] std::expected<void, StateDecodeError> done() const
    {
        if (_error)
            return StateFailure { *_error };
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

    void readKey(StateReader& in)
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
        if (id > std::numeric_limits<uint32_t>::max() || std::ranges::contains(_fields, field.id, &Field::id))
        {
            fail(StateDecodeError::MalformedRecord);
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
    void readValue(StateReader& in, T& value)
    {
        if (_error)
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
            auto inner = StateReader::ofPayload(*payload);
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
        if constexpr (std::is_same_v<T, bool>)
        {
            if (raw > 1)
                fail(StateDecodeError::InvalidValue);
            else
                value = raw != 0;
        }
        else if constexpr (std::is_enum_v<T>)
        {
            static_assert(WireEnum<T>,
                          "an enum crosses the wire by its stable table: add it to StateWire.hpp");
            constexpr auto const& Values = StateEnumWire<T>::Values;
            if (raw >= Values.size())
                fail(StateDecodeError::InvalidValue);
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
            if (!_error)
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
            fail(StateDecodeError::InvalidValue);
    }

    /// Reads @p value from the whole of @p in.
    template <typename T>
    void readPayload(StateReader& in, T& value)
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
            while (!in.atEnd() && !_error)
            {
                auto codepoint = char32_t {};
                readValue(in, codepoint);
                value.push_back(codepoint);
            }
        }
        else if constexpr (IsStdVector<T>::value)
        {
            value.clear();
            while (!in.atEnd() && !_error)
                if (spend(sizeof(typename T::value_type)))
                    readValue(in, value.emplace_back());
        }
        else if constexpr (IsStdArray<T>::value)
        {
            // A newer writer's longer array has its excess read and dropped; an older one's shorter array
            // leaves the rest at its default.
            auto index = size_t { 0 };
            while (!in.atEnd() && !_error)
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
            while (!in.atEnd() && !_error)
            {
                auto key = typename T::key_type {};
                auto mapped = typename T::mapped_type {};
                readValue(in, key);
                readValue(in, mapped);
                if (!_error && spend(sizeof(typename T::value_type))
                    && !value.emplace(std::move(key), std::move(mapped)).second)
                    fail(StateDecodeError::MalformedRecord);
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
                fail(StateDecodeError::InvalidValue);
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
            auto nested = FieldDecoder { *payload, _budget };
            nested.readFields(value);
            if (auto const done = nested.done(); !done)
                fail(done.error());
        }
        if (!_error && !in.atEnd())
            fail(StateDecodeError::MalformedRecord);
    }

    template <typename Variant, size_t Index = 0>
    void emplaceAlternative(StateReader& in, Variant& value, uint64_t index)
    {
        if constexpr (Index < std::variant_size_v<Variant>)
        {
            if (index == Index)
                readValue(in, value.template emplace<Index>());
            else
                emplaceAlternative<Variant, Index + 1>(in, value, index);
        }
        else
            fail(StateDecodeError::InvalidValue);
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
        fail(StateDecodeError::TooLarge);
        return false;
    }

    DecodeBudget& _budget;
    std::vector<Field> _fields;
    std::optional<StateDecodeError> _error;
};

// {{{ foreign types

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, Color>
void visitStateFields(Visitor& visitor, T& color)
{
    // The type crosses by its stable wire index, not as the byte the in-memory layout keeps it in.
    constexpr auto PayloadMask = uint32_t { 0x00FF'FFFF };
    constexpr auto TypeShift = 24U;
    if constexpr (std::is_same_v<Visitor, FieldDecoder>)
    {
        auto type = ColorType::Undefined;
        auto payload = uint32_t { 0 };
        visitor(1, type);
        visitor(2, payload);
        if (payload > PayloadMask)
            visitor.fail(StateDecodeError::InvalidValue);
        color.content = (static_cast<uint32_t>(type) << TypeShift) | (payload & PayloadMask);
    }
    else
    {
        visitor(1, color.type());
        visitor(2, color.content & PayloadMask);
    }
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, GraphicsAttributes>
void visitStateFields(Visitor& visitor, T& attributes)
{
    visitor(1, attributes.foregroundColor);
    visitor(2, attributes.backgroundColor);
    visitor(3, attributes.underlineColor);
    visitor(4, attributes.flags);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, RGBColor>
void visitStateFields(Visitor& visitor, T& color)
{
    visitor(1, color.red);
    visitor(2, color.green);
    visitor(3, color.blue);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, RGBAColor>
void visitStateFields(Visitor& visitor, T& color)
{
    visitor(1, color.value);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, CellLocation>
void visitStateFields(Visitor& visitor, T& location)
{
    visitor(1, location.line);
    visitor(2, location.column);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, PageSize>
void visitStateFields(Visitor& visitor, T& size)
{
    visitor(1, size.lines);
    visitor(2, size.columns);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, HistoryLimits>
void visitStateFields(Visitor& visitor, T& limits)
{
    visitor(1, limits.guaranteed);
    visitor(2, limits.capacity);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, ImageSize>
void visitStateFields(Visitor& visitor, T& size)
{
    visitor(1, size.width);
    visitor(2, size.height);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, crispy::Point>
void visitStateFields(Visitor& visitor, T& point)
{
    visitor(1, point.x);
    visitor(2, point.y);
}

// }}}

/// Writes @p value's fields as a payload of their own, with no stream header.
/// @param writer The stream.
/// @param value A state struct.
template <typename T>
void encodeFields(StateWriter& writer, T const& value)
{
    auto encoder = FieldEncoder { writer };
    encoder.writeFields(value);
}

/// Reads @p value's fields from @p payload.
/// @param payload The fields, as encodeFields() wrote them.
/// @param value   Filled in; a field the payload lacks keeps the value it had.
/// @return Nothing, or why the payload is not a valid @p value.
template <typename T>
[[nodiscard]] std::expected<void, StateDecodeError> decodeFields(std::span<uint8_t const> payload, T& value)
{
    auto budget = DecodeBudget { payload.size() };
    auto decoder = FieldDecoder { payload, budget };
    decoder.readFields(value);
    return decoder.done();
}

} // namespace vtbackend::detail
