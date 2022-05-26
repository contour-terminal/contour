/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/Cell.h>
#include <terminal/Line.h>

#include <crispy/escape.h>

#include <catch2/catch.hpp>

using namespace std;

using namespace terminal;
using namespace crispy;

TEST_CASE("Line.BufferFragment", "[Line]")
{
    auto constexpr testText = "0123456789ABCDEF"sv;
    auto pool = BufferObjectPool(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(testText);
    auto const bufferFragment = bufferObject->ref(0, 10);

    auto const externalView = string_view(bufferObject->data(), 10);
    auto const fragment = BufferFragment(bufferObject, externalView);
    CHECK(fragment.view() == externalView);
}

TEST_CASE("Line.inflate", "[Line]")
{
    auto constexpr testText = "0123456789ABCDEF"sv;
    auto pool = BufferObjectPool(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(testText);
    auto const bufferFragment = bufferObject->ref(0, 10);

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x123456);
    sgr.backgroundColor = Color::Indexed(IndexedColor::Yellow);
    sgr.underlineColor = Color::Indexed(IndexedColor::Red);
    sgr.styles |= CellFlags::CurlyUnderlined;
    auto const trivial =
        TriviallyStyledLineBuffer { ColumnCount(10), sgr, HyperlinkId {}, ColumnCount(10), bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == 10);
    for (size_t i = 0; i < inflated.size(); ++i)
    {
        auto const& cell = inflated[i];
        INFO(fmt::format("column {} codepoint {}", i, (char) cell.codepoint(0)));
        CHECK(cell.foregroundColor() == sgr.foregroundColor);
        CHECK(cell.backgroundColor() == sgr.backgroundColor);
        CHECK(cell.underlineColor() == sgr.underlineColor);
        CHECK(cell.codepointCount() == 1);
        CHECK(char(cell.codepoint(0)) == testText[i]);
    }
}
