// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::InvalidSocket` -- `INVALID_SOCKET` as a `SOCKET` rather than as the macro.
///
/// The macro expands to `(SOCKET)(~0)`, and the `~0` inside it is a signed `int`: a comparison
/// against the macro is read as a signed/unsigned one (clang-tidy's
/// `modernize-use-integer-sign-comparison`), although both sides are a `SOCKET`. A constant of the
/// right type compares as what it always meant. It is defined once here; before Task B13 the
/// completion-port files each carried a copy of their own and the rest of `windows/` compared
/// against the macro, which is how 29 of those findings existed and no Linux leg could see one.
/// Private to `core::net`.

// clang-format off
#include <winsock2.h>
// clang-format on

namespace core::net::detail
{

/// The value every `SOCKET`-returning Winsock call answers failure with.
inline constexpr SOCKET InvalidSocket = INVALID_SOCKET;

} // namespace core::net::detail
