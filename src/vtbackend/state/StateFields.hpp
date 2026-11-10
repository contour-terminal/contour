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

#include <core/codec/Fields.hpp>

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

/// The state codec's view of core::codec's field codec, with the types of this module listed on top of it.
///
/// A state struct names its fields once, in `visitFields(this auto& self, auto& visitor)`, calling
/// `visitor(id, self.field)` for each; the wire format, its tolerance rules and the encoder and decoder are
/// core::codec's (<core/codec/Fields.hpp> documents them). What this module adds is the field lists of the
/// foreign types that cannot carry the member -- `visitForeignFields(visitor, value)` overloads below, found
/// by argument-dependent lookup through StateFieldHooks -- and the stable wire tables of its enumerations and
/// flag sets (@see StateEnumWire, StateFlagWire).
namespace vtbackend::detail
{

/// The hooks tag of the state codec: the namespace of this tag is where core::codec looks for the
/// `visitForeignFields` overloads that list the fields of the foreign types.
struct StateFieldHooks
{
};

using FieldEncoder =
    core::codec::BasicFieldEncoder<StateFieldHooks>; ///< Writes the fields a state struct lists.
using FieldDecoder =
    core::codec::BasicFieldDecoder<StateFieldHooks>; ///< Reads the fields a state struct lists.
using core::codec::BoxedValue;
using core::codec::fieldKey;
using core::codec::IsStdArray;
using core::codec::IsStdOptional;
using core::codec::IsStdPair;
using core::codec::IsStdVariant;
using core::codec::IsStdVector;
using core::codec::isVarintValue;
using core::codec::ListsOwnFields;
using core::codec::MapLike;
using core::codec::WireKind;

// {{{ foreign types

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, Color>
void visitForeignFields(Visitor& visitor, T& color)
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
void visitForeignFields(Visitor& visitor, T& attributes)
{
    visitor(1, attributes.foregroundColor);
    visitor(2, attributes.backgroundColor);
    visitor(3, attributes.underlineColor);
    visitor(4, attributes.flags);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, RGBColor>
void visitForeignFields(Visitor& visitor, T& color)
{
    visitor(1, color.red);
    visitor(2, color.green);
    visitor(3, color.blue);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, RGBAColor>
void visitForeignFields(Visitor& visitor, T& color)
{
    visitor(1, color.value);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, CellLocation>
void visitForeignFields(Visitor& visitor, T& location)
{
    visitor(1, location.line);
    visitor(2, location.column);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, PageSize>
void visitForeignFields(Visitor& visitor, T& size)
{
    visitor(1, size.lines);
    visitor(2, size.columns);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, HistoryLimits>
void visitForeignFields(Visitor& visitor, T& limits)
{
    visitor(1, limits.guaranteed);
    visitor(2, limits.capacity);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, ImageSize>
void visitForeignFields(Visitor& visitor, T& size)
{
    visitor(1, size.width);
    visitor(2, size.height);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, crispy::Point>
void visitForeignFields(Visitor& visitor, T& point)
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
    core::codec::encodeFields<StateFieldHooks>(writer, value);
}

/// Reads @p value's fields from @p payload.
/// @param payload The fields, as encodeFields() wrote them.
/// @param value   Filled in; a field the payload lacks keeps the value it had.
/// @return Nothing, or why the payload is not a valid @p value.
template <typename T>
[[nodiscard]] std::expected<void, StateDecodeError> decodeFields(std::span<uint8_t const> payload, T& value)
{
    return core::codec::decodeFields<StateFieldHooks>(payload, value);
}

} // namespace vtbackend::detail
