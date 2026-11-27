// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Byte comparison whose time does not depend on where the bytes differ, and zeroing a compiler
/// may not remove: for keys, MACs and signatures, where an early exit tells an attacker how many
/// leading bytes of a guess were right, and a dead store left a secret in memory.
///
/// **What is guaranteed, and how.** Both functions are out of line, neither `constexpr` nor
/// `inline`, so no caller's optimizer sees through them. `constantTimeEquals` reads every byte of
/// both inputs, accumulates the XOR of each pair, and passes the accumulator through a value
/// barrier after every byte (an empty `asm volatile` that claims to change it, or a `volatile`
/// object under MSVC's front end), so the compiler can neither prove the result early nor turn the
/// loop into a search that stops at the first difference. Its time depends only on the length.
/// `secureZero` writes every byte through a `volatile` pointer and then passes the buffer through
/// a memory barrier, so the stores are neither elided as dead nor merged away. The barriers are
/// compiler barriers in themselves, not merely calls the optimizer cannot see into, so they hold
/// under link-time optimization too, where a call may be inlined after all. This holds against
/// the compiler; what a CPU does with a data-dependent load it never sees is outside it, and none
/// is made.
///
/// **What is not hidden: the length.** Inputs of different lengths compare unequal at once. A MAC
/// or a key has a public, fixed length; compare equal-length values.
///
/// That the compare loop has no data-dependent branch is a property of the generated code: it is
/// checked by reading the disassembly of the release builds, not by a unit test, which cannot see a
/// timing difference reliably.

#include <core/BaseExport.hpp>

#include <cstddef>
#include <span>

namespace core
{

/// Compares @p lhs and @p rhs, every byte, without an early exit. A bool: the name asks the question.
/// @param lhs One value.
/// @param rhs The other.
/// @return Whether both have the same length and the same bytes; false on a length difference, at once.
[[nodiscard]] CORE_BASE_API bool constantTimeEquals(std::span<std::byte const> lhs,
                                                    std::span<std::byte const> rhs) noexcept;

/// Overwrites @p bytes with zeros; the stores survive optimization (volatile writes and a memory
/// barrier, out of line), even when the buffer is never read again.
/// @param bytes What to clear.
CORE_BASE_API void secureZero(std::span<std::byte> bytes) noexcept;

} // namespace core
