// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

#include <vthost/WireEnum.hpp>

namespace
{
enum class Shade : uint8_t
{
    Light,
    Medium,
    Dark,
};
} // namespace

TEST_CASE("enumUpTo names each enumerator up to the last, and nothing past it", "[vthost][wire]")
{
    CHECK(vthost::enumUpTo(0, Shade::Dark) == Shade::Light);
    CHECK(vthost::enumUpTo(1, Shade::Dark) == Shade::Medium);
    CHECK(vthost::enumUpTo(2, Shade::Dark) == Shade::Dark);
    CHECK(vthost::enumUpTo(3, Shade::Dark) == std::nullopt);
    CHECK(vthost::enumUpTo(255, Shade::Dark) == std::nullopt);

    // An enumerator past the bound a decoder names reads as unknown until the bound moves: closed, not open.
    CHECK(vthost::enumUpTo(2, Shade::Medium) == std::nullopt);
}
