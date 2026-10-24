// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <ranges>

#include <vtcontrol/Uuid.hpp>

namespace vtcontrol::testing
{

/// A `UuidSource` for tests: `00000000-0000-7000-8000-000000000001`, then `...0002`, and so on.
class SequentialUuidSource final: public UuidSource
{
  public:
    /// @return The next identifier in the sequence.
    [[nodiscard]] Uuid next() override
    {
        auto uuid = Uuid {};
        uuid.bytes[6] = 0x70;
        uuid.bytes[8] = 0x80;
        ++_counter;
        for (auto const index: std::views::iota(0, 7))
            uuid.bytes[static_cast<std::size_t>(15 - index)] = static_cast<uint8_t>(_counter >> (8 * index));
        return uuid;
    }

  private:
    uint64_t _counter = 0;
};

} // namespace vtcontrol::testing
