// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <testing/ExactBuffer.hpp>

TEST_CASE("an exact buffer holds a copy of its text and nothing more", "[testing]")
{
    for (auto const text: { std::string_view { "{\"a\":1}" }, std::string_view {} })
    {
        CAPTURE(text);
        auto const buffer = contour::testing::ExactBuffer { text };
        CHECK(buffer.view() == text);
        CHECK(buffer.view().size() == text.size());
        CHECK((text.empty() || buffer.view().data() != text.data()));
    }
}
