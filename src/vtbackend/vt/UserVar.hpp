// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace vtbackend
{

/// The bounds on the user variables a program may set (OSC 1337 ; SetUserVar): a program is untrusted, and
/// the terminal keeps what it sets for the session's lifetime.
struct UserVarLimits
{
    std::size_t maxCount = 64;        ///< Distinct names remembered at most; kitty's bound.
    std::size_t maxNameBytes = 128;   ///< Longest accepted name.
    std::size_t maxValueBytes = 4096; ///< Longest accepted decoded value.
};

/// A user variable a program set, validated and decoded.
struct UserVarAssignment
{
    std::string name;  ///< The variable's name: non-empty, without control characters.
    std::string value; ///< Its decoded value.
};

/// Why a SetUserVar argument was not accepted.
enum class UserVarRejection : uint8_t
{
    MissingSeparator, ///< No `=` between the name and the value.
    InvalidName,      ///< The name is empty, too long, or holds a control character.
    InvalidValue,     ///< The value is not valid base64.
    ValueTooLong,     ///< The value decodes to more than UserVarLimits::maxValueBytes.
};

/// @param rejection A rejection reason.
/// @return Its spelling, for logs.
[[nodiscard]] std::string_view toString(UserVarRejection rejection) noexcept;

/// What became of an assignment the terminal was handed.
enum class UserVarStorage : uint8_t
{
    Dropped = 0, ///< A new name, and UserVarLimits::maxCount names are remembered already.
    Stored,      ///< Remembered: a known name updated, or a new name that found room.
};

/// Parses and decodes the argument of `OSC 1337 ; SetUserVar=<name>=<base64 value>`.
/// @param argument What follows `SetUserVar=`.
/// @param limits The bounds on name and value.
/// @return The assignment, or why the argument was rejected.
[[nodiscard]] std::expected<UserVarAssignment, UserVarRejection> decodeUserVar(std::string_view argument,
                                                                               UserVarLimits const& limits);

} // namespace vtbackend
