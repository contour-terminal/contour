// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Canonical ABI of the control vocabulary (`terminal-extensions:control@0.1.0`): how its records and
/// enums lie in a guest's memory and flatten into core-wasm values.
///
/// No row per record: the records are `vtcontrol`'s API structs, whose fields are reflected, and an enum's
/// cases are its `EnumTable` rows, so a field added to the catalog is lifted and lowered without an edit
/// here -- in the order the WIT renderer (`vtcontrol::renderControlWit`) declares it, which reads the same
/// reflection.
///
/// **Private to vtextension.** It reaches glaze through vtcontrol/Reflect.hpp, and glaze is linked
/// privately: include it from vtextension's `.cpp` files and tests only, never from a header another module
/// sees (Instance.hpp, IHostServices.hpp, ...). The `vtextension_public_headers_test` target compiles those
/// without glaze, so a header that pulled this one in would fail to build there.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/EnumTable.hpp>
#include <vtcontrol/Reflect.hpp>
#include <vtextension/CanonicalAbi.hpp>

namespace vtextension::abi
{

/// Whether `T` is a record of the control vocabulary: an API struct, or the error every verb may return.
template <typename T>
concept ControlRecord = vtcontrol::ApiRecord<T> || std::same_as<T, vtcontrol::ControlError>;

/// An enum with an `EnumTable` has one case per row. The lift maps a case's index to the enumerator of that
/// value, which is right only for a table in enumerator order: asserted here, where the codec relies on it.
template <vtcontrol::HasEnumTable E>
inline constexpr uint32_t EnumCaseCount<E> = [] {
    static_assert(vtcontrol::enumTableIsOrdered<E>(), "the codec lifts a case by its index into the table");
    return static_cast<uint32_t>(vtcontrol::EnumTable<E>::Rows.size());
}();

namespace detail
{
    /// How a record codec reaches the fields of a control record: by reflection, in declaration order.
    template <typename T>
    struct ReflectedAccess
    {
        static constexpr std::size_t FieldCount = vtcontrol::reflect::FieldCount<T>;

        template <std::size_t I>
        using Field = vtcontrol::reflect::FieldType<T, I>;

        template <std::size_t I>
        [[nodiscard]] static constexpr Field<I> const& get(T const& value) noexcept
        {
            return vtcontrol::reflect::field<I>(value);
        }

        template <std::size_t I>
        [[nodiscard]] static constexpr Field<I>& get(T& value) noexcept
        {
            return vtcontrol::reflect::field<I>(value);
        }
    };

    /// Whether every core-wasm value `T` flattens to is an i32, as everything of the control vocabulary must
    /// be (7a's rule: no floating-point and no 64-bit integer in an API struct). A list's elements are
    /// checked too, though they travel in memory, so that the rule holds for the vocabulary as a whole.
    template <typename T>
    [[nodiscard]] consteval bool flattensToI32()
    {
        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>
                      || std::is_same_v<T, vtcontrol::RawJson>)
            return true;
        else if constexpr (std::is_integral_v<T>)
            return sizeof(T) <= sizeof(uint32_t);
        else if constexpr (std::is_enum_v<T>)
            return EnumCaseCount<T> > 0;
        else if constexpr (vtcontrol::reflect::IsOptional<T> || vtcontrol::reflect::IsVector<T>)
            return flattensToI32<typename T::value_type>();
        else if constexpr (ControlRecord<T>)
            return []<std::size_t... I>(std::index_sequence<I...>) {
                return (true && ... && flattensToI32<vtcontrol::reflect::FieldType<T, I>>());
            }(std::make_index_sequence<vtcontrol::reflect::FieldCount<T>> {});
        else
            return false;
    }
} // namespace detail

/// The control vocabulary's records: their reflected fields, each at its natural alignment, in order.
template <ControlRecord T>
struct Codec<T>: detail::RecordCodec<T, detail::ReflectedAccess<T>>
{
};

/// `vtcontrol::RawJson` is a WIT `string` holding the JSON text.
template <>
struct Codec<vtcontrol::RawJson>
{
    using Text = Codec<std::string>;
    static constexpr uint32_t Size = Text::Size;
    static constexpr uint32_t Alignment = Text::Alignment;
    static constexpr std::size_t FlatCount = Text::FlatCount;

    [[nodiscard]] static Result<void> store(IGuestMemory& memory,
                                            Address address,
                                            vtcontrol::RawJson const& value)
    {
        return Text::store(memory, address, value.text);
    }

    [[nodiscard]] static Result<vtcontrol::RawJson> load(IGuestMemory& memory, Address address)
    {
        return Text::load(memory, address).transform([](std::string text) {
            return vtcontrol::RawJson { .text = std::move(text) };
        });
    }

    [[nodiscard]] static Result<void> lowerFlat(IGuestMemory& memory,
                                                vtcontrol::RawJson const& value,
                                                FlatValues& out)
    {
        return Text::lowerFlat(memory, value.text, out);
    }

    [[nodiscard]] static Result<vtcontrol::RawJson> liftFlat(IGuestMemory& memory, FlatReader& reader)
    {
        return Text::liftFlat(memory, reader).transform([](std::string text) {
            return vtcontrol::RawJson { .text = std::move(text) };
        });
    }
};

/// Whether every request and reply of the catalog, and the error, flattens to i32 values only.
inline constexpr bool ControlVocabularyFlattensToI32 = [] {
    return std::apply(
               [](auto const&... row) {
                   return (
                       true && ...
                       && (detail::flattensToI32<typename std::remove_cvref_t<decltype(row)>::Request>()
                           && detail::flattensToI32<typename std::remove_cvref_t<decltype(row)>::Reply>()));
               },
               vtcontrol::Methods)
           && detail::flattensToI32<vtcontrol::ControlError>();
}();

static_assert(
    ControlVocabularyFlattensToI32,
    "a control API struct holds a member that does not flatten to i32 (a float or a 64-bit integer)");

} // namespace vtextension::abi
