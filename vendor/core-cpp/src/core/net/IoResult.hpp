// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Result and error types for the async socket layer. All fallible socket I/O
/// resolves to an @c IoResult (`std::expected<std::size_t, NetError>`): the value
/// is the number of bytes transferred (0 on a clean EOF for reads), the error is a
/// structured @c NetError.

#include <core/net/NetError.hpp>

#include <cstddef>
#include <expected>

namespace core::net
{

/// Result of a byte-transfer operation: the count transferred, or a @c NetError.
using IoResult = std::expected<std::size_t, NetError>;

} // namespace core::net
