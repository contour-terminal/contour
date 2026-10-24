// SPDX-License-Identifier: Apache-2.0
//
// Test-only: a guest's linear memory without a guest, for driving the Canonical ABI codec directly.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/ExtensionError.hpp>

namespace vtextension::testing
{

/// A linear memory of its own: a byte array, and a bump allocator in place of `cabi_realloc`.
class FakeGuestMemory final: public abi::IGuestMemory
{
  public:
    static constexpr uint32_t Size = 64 * 1024;

    /// @param liftBudget What lifting may spend on the host, in all.
    explicit FakeGuestMemory(uint64_t liftBudget = std::numeric_limits<uint64_t>::max()):
        _liftBudget { liftBudget }
    {
    }

    [[nodiscard]] abi::Result<void> chargeLift(uint64_t bytes) override
    {
        if (bytes > _liftBudget)
            return std::unexpected(ExtensionError::Trapped);
        _liftBudget -= bytes;
        return {};
    }

    [[nodiscard]] abi::Result<std::span<std::byte>> view(abi::Address address, uint32_t size) override
    {
        if (uint64_t { address } + size > _bytes.size())
            return std::unexpected(ExtensionError::Trapped);
        return std::span { _bytes }.subspan(address, size);
    }

    [[nodiscard]] abi::Result<abi::Address> allocate(uint32_t size, uint32_t alignment) override
    {
        auto const address = abi::alignTo(_next, alignment);
        if (uint64_t { address } + size > _bytes.size())
            return std::unexpected(ExtensionError::OutOfMemory);
        _next = address + size;
        return address;
    }

    /// Writes @p value at @p address as a little-endian u32, as a guest would.
    void poke(abi::Address address, uint32_t value)
    {
        REQUIRE(abi::Codec<uint32_t>::store(*this, address, value).has_value());
    }

  private:
    std::vector<std::byte> _bytes = std::vector<std::byte>(Size);
    abi::Address _next = 8; ///< Address 0 stays unused, as a guest's null.
    uint64_t _liftBudget;
};

} // namespace vtextension::testing
