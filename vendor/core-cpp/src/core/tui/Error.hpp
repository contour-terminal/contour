// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace core::tui
{

/// @brief Error codes for TUI operations.
enum class ErrorCode : std::uint8_t
{
    IoError,
    InvalidArgument,
};

/// @brief Result type for operations that return a value or fail.
template <typename T>
using Result = std::expected<T, std::string>;

/// @brief Result type for operations that can fail without returning a value.
using VoidResult = std::expected<void, std::string>;

/// @brief Creates an error result with the given message.
/// @param code The error code (currently unused, kept for API compatibility).
/// @param message The error message.
/// @return An unexpected result containing the error message.
inline auto makeError([[maybe_unused]] ErrorCode code, std::string_view message) -> VoidResult
{
    return std::unexpected(std::string(message));
}

} // namespace core::tui
