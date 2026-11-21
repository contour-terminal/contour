// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

#include <orchestrator/core/Uuid.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

TEST_CASE("formatUuidV4 sets version and variant", "[orchestrator][uuid]")
{
    struct Row
    {
        uint8_t fill = 0;
        std::string_view expected;
    };
    static constexpr auto Rows = std::array {
        Row { .fill = 0x00, .expected = "00000000-0000-4000-8000-000000000000"sv },
        Row { .fill = 0xff, .expected = "ffffffff-ffff-4fff-bfff-ffffffffffff"sv },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.fill);
        auto bytes = std::array<uint8_t, 16> {};
        bytes.fill(row.fill);
        CHECK(formatUuidV4(bytes) == row.expected);
    }

    // Every other bit is the source's, in order.
    auto const counting = std::array<uint8_t, 16> { 0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x12, 0xd3,
                                                    0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00 };
    CHECK(formatUuidV4(counting) == "123e4567-e89b-42d3-a456-426614174000"sv);
}
