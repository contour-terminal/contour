// SPDX-License-Identifier: Apache-2.0
#include <core/Random.hpp>

#include <core/SystemRandomSource.hpp>

namespace core
{

namespace
{
    class SystemRandom final: public IRandom
    {
      public:
        void fill(std::span<std::byte> out) override { detail::fillFromSystemRandom(out); }
    };
} // namespace

IRandom& systemRandom()
{
    static auto instance = SystemRandom {};
    return instance;
}

} // namespace core
