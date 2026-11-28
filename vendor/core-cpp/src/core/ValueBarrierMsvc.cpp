// SPDX-License-Identifier: Apache-2.0
//
// The barriers for MSVC's front end (cl and clang-cl), which has no GNU inline assembly on x64 or
// arm64. The value barrier is a volatile object, whose every read and write the compiler must
// perform as written, so a value passed through one is unknown to it afterwards. A volatile access
// orders only other volatile accesses, so the memory barrier is a signal fence: a compiler fence
// that emits no instruction and keeps every memory access before it from being sunk past it, with
// the pointer stored to a volatile object first so the memory it names is taken to escape.
#include <core/ValueBarrier.hpp>

#include <atomic>
#include <cstdint>

namespace core::detail
{

std::uint8_t valueBarrier(std::uint8_t value) noexcept
{
    auto volatile hidden = value;
    return hidden;
}

void memoryBarrier(void const* memory) noexcept
{
    auto const* volatile hidden = memory;
    static_cast<void>(hidden);
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

} // namespace core::detail
