// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The global allocation functions a counting test replaces, forwarded to two functions the test
/// defines.
///
/// A test that counts or fails allocations links `ReplacedGlobalAllocation.cpp` and defines
/// @c replacedAllocate and @c replacedRelease. The replacements live in that translation unit,
/// which includes nothing but this header, rather than in the test's own. That keeps them away
/// from `<new>`, whose MSVC declarations name the pointer `_Block`, a name no parameter here may
/// take. It also keeps a static analyser from pairing a `new` it sees in the test with a `free` it
/// would see inlined from the replacement.

#include <cstddef>

namespace core::testing
{

/// Allocates @p size bytes for every replaced `operator new` and `operator new[]`.
/// @param size The bytes asked for.
/// @return The storage. Throws `std::bad_alloc` where the test refuses the allocation.
[[nodiscard]] void* replacedAllocate(std::size_t size);

/// Releases what @c replacedAllocate returned, for every replaced `operator delete` and
/// `operator delete[]`, sized or not.
/// @param storage The storage, or null.
void replacedRelease(void* storage) noexcept;

} // namespace core::testing
