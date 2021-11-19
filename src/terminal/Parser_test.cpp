/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <terminal/Parser.h>
#include <terminal/ParserEvents.h>
#include <unicode/convert.h>
#include <catch2/catch_all.hpp>

using namespace std;
using namespace terminal;

class MockParserEvents : public terminal::BasicParserEvents {
  public:
    std::string text;
    std::string apc;
    std::string pm;

    void error(string_view const& _msg) override { INFO(fmt::format("Parser error received. {}", _msg)); }
    void print(char ch) override { text += ch; }
    void print(std::string_view s) override { text += s; }

    void startAPC() override { apc += "{"; }
    void putAPC(char ch) override { apc += ch; }
    void dispatchAPC() override { apc += "}"; }

    void startPM() override { pm += "{"; }
    void putPM(char32_t ch) override { pm += unicode::convert_to<char>(ch); }
    void dispatchPM() override { pm += "}"; }
};

TEST_CASE("Parser.utf8_single", "[Parser]")
{
    MockParserEvents textListener;
    auto p = parser::Parser(textListener);

    p.parseFragment("\xC3\xB6");  // ö

    REQUIRE(textListener.text == "\xC3\xB6");
}

TEST_CASE("Parser.PM")
{
    MockParserEvents listener;
    auto p = parser::Parser(listener);
    REQUIRE(p.state() == parser::State::Ground);
    p.parseFragment("ABC\033^hello\033\\DEF"sv);
    CHECK(p.state() == parser::State::Ground);
    CHECK(listener.pm == "{hello}");
    CHECK(listener.text == "ABCDEF");
}

TEST_CASE("Parser.APC")
{
    MockParserEvents listener;
    auto p = parser::Parser(listener);
    REQUIRE(p.state() == parser::State::Ground);
    p.parseFragment("ABC\033\\\033_Gi=1,a=q;\033\\DEF"sv);
    REQUIRE(p.state() == parser::State::Ground);
    REQUIRE(listener.apc == "{Gi=1,a=q;}");
    REQUIRE(listener.text == "ABCDEF");
}

