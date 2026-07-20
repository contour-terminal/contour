// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string_view>

namespace crispy::environment
{

/// Reads a variable from the process environment.
///
/// The environment is an ambient global that `setenv()`/`putenv()` may mutate, which makes the
/// plain `getenv()` unsafe to call concurrently. We therefore snapshot the environment once, on
/// first access, and serve every lookup from that immutable copy — so reads are thread safe and
/// the returned view stays valid for the lifetime of the process.
///
/// This trades away visibility of environment changes made after the first lookup. Nothing in
/// this codebase mutates its own environment, so that trade is free; a process that does must
/// not use this API.
///
/// @param name Name of the variable to look up.
/// @return The variable's value, or std::nullopt if it is not set.
[[nodiscard]] std::optional<std::string_view> get(std::string_view name);

/// Like get(), but yields a NUL-terminated C string for APIs that require one.
///
/// @param name Name of the variable to look up.
/// @return Pointer into the immortal environment snapshot, or nullptr if @p name is not set.
[[nodiscard]] char const* getCString(std::string_view name);

} // namespace crispy::environment
