// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The control API's JSON: the one header of `vtcontrol` that names glaze. The vocabulary headers stay free
/// of it, so a header that names an API type costs its includers nothing; what glaze must know about them --
/// the spelling of every `EnumTable` enum, `RawJson`, the string writer -- is taught here. Include it from
/// `.cpp` files only, and only where JSON is read or written: a translation unit that reflects an API type
/// without it would spell enums by number.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <glaze/json.hpp>
#include <vtcontrol/Api.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/EnumTable.hpp>

namespace vtcontrol
{

/// The glaze options every control JSON document is read and written with.
///
/// Reading is strict about what is there (an unknown key or a wrong type is an error) and lenient
/// about what is not (an absent key keeps the member's default). Writing leaves out empty optionals.
inline constexpr auto JsonOptions =
    glz::opts { .error_on_unknown_keys = true, .skip_null_members = true, .error_on_missing_keys = false };

/// @param value An API struct.
/// @return Its compact JSON, with the keys spelled like the members.
template <typename T>
[[nodiscard]] std::string encodeJson(T const& value)
{
    auto text = std::string {};
    // Writing can only fail for a type glaze cannot reflect, which fails to compile instead.
    (void) glz::write<JsonOptions>(value, text);
    return text;
}

/// @param text A JSON document; an empty one reads as `{}`.
/// @return The decoded struct, or `BadRequest` whose message is glaze's error text (naming the key).
template <typename T>
[[nodiscard]] std::expected<T, ControlError> decodeJson(std::string_view text)
{
    auto value = T {};
    auto const document = text.empty() ? std::string_view { "{}" } : text;
    if (auto const error = glz::read<JsonOptions>(value, document))
        return std::unexpected(ControlError { .code = ControlErrorCode::BadRequest,
                                              .message = glz::format_error(error, document),
                                              .candidates = {} });
    return value;
}

/// @param text Arbitrary bytes.
/// @return `text` with every invalid UTF-8 sequence replaced by U+FFFD, so that it can sit in a JSON string.
[[nodiscard]] std::string toValidUtf8(std::string_view text);

/// @param text Arbitrary bytes, e.g. a pane's text with ESC and NUL in it.
/// @return A JSON string literal, quotes included: invalid UTF-8 becomes U+FFFD and every control
///         character is escaped, so the document stays valid whatever the pane printed.
[[nodiscard]] std::string quoteJsonString(std::string_view text);

} // namespace vtcontrol

/// Strings are written through `vtcontrol::quoteJsonString`. Glaze's own writer does not escape most
/// control characters (it emits NUL bytes to flag them), which would let one pane's output break a
/// whole reply. This is a full specialization, so it wins over every glaze rule, and it lives here so
/// that every translation unit that writes JSON sees it.
///
/// It names glaze internals (`glz::detail::to_json`, `glz::detail::dump_maybe_empty`, `is_context`), which
/// glaze does not promise to keep: on a glaze bump (`GLAZE_VERSION` in cmake/ContourThirdParties.cmake),
/// re-run `vtcontrol_test "[json]"`, whose control-character cases fail if this stops being chosen.
template <>
struct glz::detail::to_json<std::string>
{
    template <auto Opts, class B>
    static void op(std::string const& value, is_context auto&& /*ctx*/, B& buffer, auto& index) noexcept
    {
        auto const quoted = vtcontrol::quoteJsonString(value);
        glz::detail::dump_maybe_empty(quoted, buffer, index);
    }
};

/// Teaches glaze the spelling of every enum that has an `EnumTable`.
template <vtcontrol::HasEnumTable E>
struct glz::meta<E> // NOLINT(readability-identifier-naming)
{
    static_assert(vtcontrol::enumTableIsOrdered<E>(), "EnumTable rows must be in enumerator order from 0");

    // Glaze's `enumerate` takes alternating name and value arguments.
    static constexpr auto value = []<std::size_t... Index>(std::index_sequence<Index...>) {
        constexpr auto const& rows = vtcontrol::EnumTable<E>::Rows;
        return glz::enumerate([&]() constexpr {
            if constexpr (Index % 2 == 0)
                return rows[Index / 2].name;
            else
                return rows[Index / 2].value;
        }()...);
    }(std::make_index_sequence<vtcontrol::EnumTable<E>::Rows.size() * 2> {});
};

/// `vtcontrol::RawJson` is written as the text it holds and read as the exact text of one value, as glaze's
/// own `raw_json` is.
template <>
struct glz::detail::to_json<vtcontrol::RawJson>
{
    template <auto Opts, class B>
    static void op(vtcontrol::RawJson const& value,
                   is_context auto&& /*ctx*/,
                   B& buffer,
                   auto& index) noexcept
    {
        glz::detail::dump_maybe_empty(value.text, buffer, index);
    }
};

template <>
struct glz::detail::from_json<vtcontrol::RawJson>
{
    template <auto Opts>
    static void op(vtcontrol::RawJson& value, is_context auto&& ctx, auto&& it, auto&& end) noexcept
    {
        auto const start = it;
        glz::detail::skip_value<Opts>(ctx, it, end);
        if (bool(ctx.error))
            return;
        value.text.assign(start, static_cast<std::size_t>(it - start));
    }
};
