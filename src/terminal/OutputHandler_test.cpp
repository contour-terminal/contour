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
#include <terminal/OutputHandler.h>
#include <terminal/Parser.h>
#include <catch2/catch.hpp>
#include <fmt/format.h>

using namespace std;
using namespace terminal;

TEST_CASE("parseColor", "[OutputHandler]")
{
    Color const c1 = OutputHandler::parseColor("rgb:FFFF/FFFF/FFFF").value();
    CHECK(c1 == RGBColor{0xFF, 0xFF, 0xFF});

    Color const c2 = OutputHandler::parseColor("rgb:0000/0000/0000").value();
    CHECK(c2 == RGBColor{0x00, 0x00, 0x00});

    RGBColor const c3 = RGBColor{0x10, 0x30, 0xC0};
    std::string const s3 = setDynamicColorValue(c3);
    RGBColor const c4 = *OutputHandler::parseColor(s3);
    CHECK(c3 == c4);

    // TODO: other colors
}

TEST_CASE("utf8_single", "[OutputHandler]")  // TODO: move to Parser_test
{
    auto output = OutputHandler{[&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{ref(output)};

    parser.parseFragment("\xC3\xB6");  // ö

    REQUIRE(1 == output.commands().size());

    Command const cmd = output.commands()[0];
    REQUIRE(holds_alternative<AppendChar>(cmd));
    AppendChar const& ch = get<AppendChar>(cmd);

    REQUIRE(0xF6 == static_cast<unsigned>(ch.ch));
}

TEST_CASE("utf8_middle", "[OutputHandler]")  // TODO: move to Parser_test
{
    auto output = OutputHandler{
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("parser: {}", msg)); }};

    parser.parseFragment("A\xC3\xB6Z");  // AöZ

    REQUIRE(3 == output.commands().size());

    REQUIRE(holds_alternative<AppendChar>(output.commands()[0]));
    REQUIRE('A' == static_cast<unsigned>(get<AppendChar>(output.commands()[0]).ch));

    REQUIRE(holds_alternative<AppendChar>(output.commands()[1]));
    REQUIRE(0xF6 == static_cast<unsigned>(get<AppendChar>(output.commands()[1]).ch));

    REQUIRE(holds_alternative<AppendChar>(output.commands()[2]));
    REQUIRE('Z' == static_cast<unsigned>(get<AppendChar>(output.commands()[2]).ch));
}

TEST_CASE("set_g1_special", "[OutputHandler]")
{
    auto output = OutputHandler{
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};

    parser.parseFragment("\033)0");
    REQUIRE(1 == output.commands().size());
    REQUIRE(holds_alternative<DesignateCharset>(output.commands()[0]));
    auto ct = get<DesignateCharset>(output.commands()[0]);
    REQUIRE(CharsetTable::G1 == ct.table);
    REQUIRE(Charset::Special == ct.charset);
}

TEST_CASE("color_fg_indexed", "[OutputHandler]")
{
    auto output = OutputHandler{
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};

    parser.parseFragment("\033[38;5;235m");
    REQUIRE(1 == output.commands().size());
    INFO(fmt::format("sgr: {}", to_string(output.commands()[0])));
    REQUIRE(holds_alternative<SetForegroundColor>(output.commands()[0]));
    auto sgr = get<SetForegroundColor>(output.commands()[0]);
    REQUIRE(holds_alternative<IndexedColor>(sgr.color));
    auto indexedColor = get<IndexedColor>(sgr.color);
    REQUIRE(235 == static_cast<unsigned>(indexedColor));
}

TEST_CASE("color_bg_indexed", "[OutputHandler]")
{
    auto output = OutputHandler{
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};

    parser.parseFragment("\033[48;5;235m");
    REQUIRE(1 == output.commands().size());
    INFO(fmt::format("sgr: {}", to_string(output.commands()[0])));
    REQUIRE(holds_alternative<SetBackgroundColor>(output.commands()[0]));
    auto sgr = get<SetBackgroundColor>(output.commands()[0]);
    REQUIRE(holds_alternative<IndexedColor>(sgr.color));
    auto indexedColor = get<IndexedColor>(sgr.color);
    REQUIRE(235 == static_cast<unsigned>(indexedColor));
}

TEST_CASE("SETMARK", "[OutputHandler]")
{
    auto output = OutputHandler{
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("{}", msg)); }};

    parser.parseFragment("\033[>M");
    REQUIRE(output.commands().size() == 1);
    INFO(fmt::format("cmd: {}", to_string(output.commands()[0])));
    REQUIRE(holds_alternative<SetMark>(output.commands()[0]));
}
