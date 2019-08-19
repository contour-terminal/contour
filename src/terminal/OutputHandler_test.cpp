// This file is part of the "libterminal" project, http://github.com/christianparpart/libterminal>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <terminal/OutputHandler.h>
#include <terminal/Parser.h>
#include <catch2/catch.hpp>
#include <fmt/format.h>

using namespace std;
using namespace terminal;

constexpr size_t RowCount = 25;

TEST_CASE("utf8_single", "[OutputHandler]")  // TODO: move to Parser_test
{
    auto output = OutputHandler{
            RowCount,
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[debug] parser: {}", msg)); },
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[trace] parser: {}", msg)); }};

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
            RowCount,
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[debug] parser: {}", msg)); },
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[trace] parser: {}", msg)); }};

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
            RowCount,
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[debug] parser: {}", msg)); },
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[trace] parser: {}", msg)); }};

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
            RowCount,
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[debug] parser: {}", msg)); },
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[trace] parser: {}", msg)); }};

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
            RowCount,
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[OutputHandler]: {}", msg)); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[debug] parser: {}", msg)); },
            [&](auto const& msg) { UNSCOPED_INFO(fmt::format("[trace] parser: {}", msg)); }};

    parser.parseFragment("\033[48;5;235m");
    REQUIRE(1 == output.commands().size());
    INFO(fmt::format("sgr: {}", to_string(output.commands()[0])));
    REQUIRE(holds_alternative<SetBackgroundColor>(output.commands()[0]));
    auto sgr = get<SetBackgroundColor>(output.commands()[0]);
    REQUIRE(holds_alternative<IndexedColor>(sgr.color));
    auto indexedColor = get<IndexedColor>(sgr.color);
    REQUIRE(235 == static_cast<unsigned>(indexedColor));
}
