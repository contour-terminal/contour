/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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
#include <crispy/text/GraphemeSegmenter.h>
#include <crispy/text/EmojiSegmenter.h>
#include <catch2/catch.hpp>

using namespace crispy::text;
using namespace std::string_literals;
using namespace std;

TEST_CASE("EmojiSegmenter.emoji.simple1", "[EmojiSegmenter]")
{
    auto const codepoints = u32string_view{U"\U0001f600"};
    auto es = EmojiSegmenter{ codepoints };

    CHECK(*es == U"\U0001f600");
    CHECK(es.isEmoji());

    ++es;
    CHECK(*es == U"");
}

TEST_CASE("EmojiSegmenter.emoji.simple2", "[EmojiSegmenter]")
{
    auto const codepoints = u32string_view{U"\U0001f600\U0001f600"};
    auto es = EmojiSegmenter{ codepoints };

    CHECK(es.isEmoji());
    CHECK(*es == U"\U0001f600");

    ++es;
    CHECK(*es == U"\U0001f600");

    ++es;
    CHECK(*es == U"");
}

TEST_CASE("EmojiSegmenter.text.simple1", "[EmojiSegmenter]")
{
    auto const codepoints = u32string_view{U"\u270c\ufe0e"};
    auto es = EmojiSegmenter{ codepoints };

    CHECK(es.isText());
    CHECK(*es == U"\u270c\ufe0e");

    ++es;
    CHECK(*es == U"");
}

TEST_CASE("EmojiSegmenter.emoji.text.emoji", "[EmojiSegmenter]")
{
    auto const codepoints = u32string_view{U"\u270c\u270c\ufe0e\u270c"};
    auto es = EmojiSegmenter{ codepoints };

    CHECK(es.isEmoji());
    CHECK(*es == U"\u270c");

    ++es;
    CHECK(es.isText());
    CHECK(*es == U"\u270c\ufe0e");

    ++es;
    CHECK(es.isEmoji());
    CHECK(*es == U"\u270c");

    ++es;
    CHECK(*es == U"");
}

TEST_CASE("EmojiSegmenter.mixed_complex", "[EmojiSegmenter]")
{
    auto const codepoints = u32string_view{
        U"\u270c"                                       // ✌ Waving hand
        U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F"       // 🤦🏼‍♂️ Face Palm
        U"\u270c\ufe0e"                                 // ✌ Waving hand (text presentation)
        U"\u270c"                                       // ✌ Waving hand
    };
    auto es = EmojiSegmenter{ codepoints };

    CHECK(es.isEmoji());
    CHECK(*es == U"\u270c");

    ++es;
    CHECK(es.isEmoji());
    CHECK(*es == U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F");

    ++es;
    CHECK(es.isText());
    CHECK(*es == U"\u270c\ufe0e");

    ++es;
    CHECK(es.isEmoji());
    CHECK(*es == U"\u270c");

    ++es;
    CHECK(*es == U"");
}
