// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The barriers ConstantTime.cpp is built on, one definition per compiler front end, chosen by CMake
/// (`ValueBarrierGnu.cpp` for GCC and Clang, `ValueBarrierMsvc.cpp` for MSVC and clang-cl). Each is
/// out of line in a source of its own, so no caller can see that it changes nothing. Private to
/// the base module.

#include <cstdint>

namespace core::detail
{

/// @param value Any value.
/// @return @p value, which the compiler must treat as unknown: nothing about it is proven across
///         the call.
[[nodiscard]] std::uint8_t valueBarrier(std::uint8_t value) noexcept;

/// Makes the compiler assume the memory behind @p memory is read here, so stores to it before the
/// call are neither elided nor sunk past it.
/// @param memory The start of the memory.
void memoryBarrier(void const* memory) noexcept;

} // namespace core::detail
