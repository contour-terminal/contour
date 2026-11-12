// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A deterministic core::IRandom for tests.

#include <core/Random.hpp>

#include <cstddef>
#include <span>

namespace vthost::testing
{

/// Counts up from one, byte by byte, so that whatever it names is known and never repeats within a test.
class CountingRandom final: public core::IRandom
{
  public:
    void fill(std::span<std::byte> out) override
    {
        for (auto& byte: out)
            byte = static_cast<std::byte>(++_next);
    }

  private:
    unsigned _next = 0;
};

} // namespace vthost::testing
