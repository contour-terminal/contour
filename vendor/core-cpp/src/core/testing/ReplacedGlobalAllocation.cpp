// SPDX-License-Identifier: Apache-2.0
//
// The replaced global allocation functions, and nothing else: see ReplacedGlobalAllocation.hpp for
// why they are kept apart from the test that counts them.
#include <core/testing/ReplacedGlobalAllocation.hpp>

#include <cstddef>

void* operator new(std::size_t size)
{
    return core::testing::replacedAllocate(size);
}

void* operator new[](std::size_t size)
{
    return core::testing::replacedAllocate(size);
}

void operator delete(void* storage) noexcept
{
    core::testing::replacedRelease(storage);
}

void operator delete[](void* storage) noexcept
{
    core::testing::replacedRelease(storage);
}

void operator delete(void* storage, std::size_t /*size*/) noexcept
{
    core::testing::replacedRelease(storage);
}

void operator delete[](void* storage, std::size_t /*size*/) noexcept
{
    core::testing::replacedRelease(storage);
}
