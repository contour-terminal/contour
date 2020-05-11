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
#include <catch2/catch.hpp>
#include <crispy/text/GraphemeSegmenter.h>

using namespace crispy::text;
using namespace std::string_literals;
using namespace std;

// TODO
// Implement examples from table 1a) at:
// http://www.unicode.org/reports/tr29/tr29-27.html#Grapheme_Cluster_Boundary_Rules

TEST_CASE("combining character sequences", "[GraphemeSegmenter]")
{
    //auto constexpr text = u32string_view{U"\u0067G\u0308"};

    CHECK(GraphemeSegmenter::nonbreakable('g', U'\u0308'));
}

// TEST_CASE("Extended grapheme clusters", "[GraphemeSegmenter]")
// {
//     // TODO: Hangul Syllables support, can't enable this test yet
//     CHECK(GraphemeSegmenter::nonbreakable(U'\u0BA8', U'\u0BBF'));   // Tamil ni
//     CHECK(GraphemeSegmenter::nonbreakable(U'\u0E40', 'e'));         // Thai e
//     CHECK(GraphemeSegmenter::nonbreakable(U'\u0E01', U'\u0E33'));   // Thai kam
//     CHECK(GraphemeSegmenter::nonbreakable(U'\u0937', U'\u093F'));   // Devanagari ssi
// }

TEST_CASE("emoji.speaking-eye", "[GraphemeSegmenter]")
{
    /*
    👁 U+1F441     Eye
    ️  U+FE0F      VS16
      U+200D      ZWJ
    🗨 U+1F5E8     Left Speech Bubble
     ️ U+FE0F      VS16
     */
    auto const zwj = u32string_view{U"\U0001F441\uFE0F\u200D\U0001F5E8\uFE0F"};
    CHECK(GraphemeSegmenter::nonbreakable(zwj[0], zwj[1]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[1], zwj[2]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[2], zwj[3]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[3], zwj[4]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[4], zwj[5]));
}

TEST_CASE("emoji", "[GraphemeSegmenter]")
{
    // 👨‍🦰
    auto const zwj = u32string_view{U"\U0001F468\u200D\U0001F9B0"};
    CHECK(GraphemeSegmenter::nonbreakable(zwj[0], zwj[1]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[1], zwj[2]));

    // 👨‍👩‍👧
    auto const zwj3 = u32string_view{U"\U0001F468\u200D\U0001F469\u200D\U0001F467"};
    CHECK(GraphemeSegmenter::nonbreakable(zwj3[0], zwj3[1]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj3[1], zwj3[2]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj3[2], zwj3[3]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj3[3], zwj3[4]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj3[4], zwj3[5]));
}

TEST_CASE("emoji: Man Facepalming: Medium-Light Skin Tone", "[GraphemeSegmenter]")
{

    auto const zwj = u32string_view{U"\U0001F926\U0001F3FC\u200D\u2642\uFE0F"};
    CHECK(GraphemeSegmenter::nonbreakable(zwj[0], zwj[1]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[1], zwj[2]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[2], zwj[3]));
    CHECK(GraphemeSegmenter::nonbreakable(zwj[3], zwj[4]));
}
