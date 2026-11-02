// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp` tool table internals, exposed so they can be tested on synthetic input.
#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace contour::cli::detail
{

/// Replaces every `{"$ref":"#/$defs/X"}` of a JSON Schema by the definition `X` and drops the `$defs`
/// member, because some MCP clients reject references. Definitions may name one another, in any order;
/// `$defs` is read from the schema's top level. Members written beside a `$ref` refine the definition.
/// @param schema The schema text.
/// @return The self-contained schema, or why it cannot be made so (malformed schema or `$defs`,
///         a reference to an unknown definition, nesting too deep or cyclic).
[[nodiscard]] std::expected<std::string, std::string> inlineSchemaRefs(std::string_view schema);

} // namespace contour::cli::detail
