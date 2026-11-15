// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <string_view>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Model.hpp>

/// Reading one JSON document with glaze. This header pulls glaze in: only .cpp files include it.
namespace orchestrator::json
{

/// Accepts any one complete JSON document, and nothing after it but whitespace. The input is a
/// string_view, so nothing terminates it.
inline constexpr auto ValidateOptions = glz::opts_validate { { glz::opts { .null_terminated = false } } };

/// Reads @p text as exactly one JSON document of type @p T.
///
/// The document is validated as a whole before it is read, and that is load-bearing: a typed read stops
/// once it has read the value, so on its own it would accept bytes after it (`{…}garbage`, or a second
/// document). A typed read does refuse a document cut off midway; a read into a generic value does not,
/// and the validation covers that as well.
/// @tparam Options The read options: at least `.null_terminated = false`.
/// @param text What a command printed, or what was stored.
/// @return The value; NotJson if @p text is not one complete JSON document, WrongShape if it is no @p T.
template <auto Options, typename T>
[[nodiscard]] std::expected<T, ParseError> readDocument(std::string_view text)
{
    auto anyValue = glz::skip {};
    if (glz::read<ValidateOptions>(anyValue, text))
        return std::unexpected(ParseError::NotJson);
    auto value = T {};
    if (glz::read<Options>(value, text))
        return std::unexpected(ParseError::WrongShape);
    return value;
}

} // namespace orchestrator::json
