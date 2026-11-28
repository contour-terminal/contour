// SPDX-License-Identifier: Apache-2.0
//
// The barriers for the GNU front end (GCC, Clang, Apple Clang, Emscripten, WASI): an empty asm
// statement that the compiler must assume reads and changes its operand, or reads all memory.
#include <core/ValueBarrier.hpp>

#include <cstdint>

namespace core::detail
{

std::uint8_t valueBarrier(std::uint8_t value) noexcept
{
    asm volatile("" : "+r"(value));
    return value;
}

void memoryBarrier(void const* memory) noexcept
{
    asm volatile("" : : "r"(memory) : "memory");
}

} // namespace core::detail
