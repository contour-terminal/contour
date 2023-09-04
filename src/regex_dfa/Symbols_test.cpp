// This file is part of the "x0" project, http://github.com/christianparpart/x0>
//   (c) 2009-2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Symbols.h>

#include <catch2/catch.hpp>

using namespace std;
using regex_dfa::SymbolSet;

TEST_CASE("regex_SymbolSet.s0")
{
    SymbolSet s0;
    REQUIRE(0 == s0.size()); // NOLINT(readability-container-size-empty)
    REQUIRE(s0.empty());
}

TEST_CASE("regex_SymbolSet.s1")
{
    SymbolSet s1;

    // first add
    s1.insert('a');
    CHECK(1 == s1.size());
    REQUIRE_FALSE(s1.empty());

    // overwrite
    s1.insert('a');
    CHECK(1 == s1.size());
    REQUIRE_FALSE(s1.empty());
}

TEST_CASE("regex_SymbolSet.initializer_list")
{
    SymbolSet a { 'a' };
    CHECK(1 == a.size());
    CHECK(a.contains('a'));

    SymbolSet s2 { 'a', 'b', 'b', 'c' };
    CHECK(3 == s2.size());
    CHECK("abc" == s2.to_string());
}

TEST_CASE("regex_SymbolSet.dot")
{
    SymbolSet dot(SymbolSet::Dot);
    REQUIRE(!dot.contains('\n'));
    CHECK(dot.contains('\0'));
    CHECK(dot.contains(' '));
    CHECK(dot.isDot());
    CHECK("." == dot.to_string());
}

TEST_CASE("regex_SymbolSet.complement")
{
    SymbolSet s;
    s.insert('\n');
    CHECK("\\n" == s.to_string());
    s.complement();
    CHECK("." == s.to_string());
}

TEST_CASE("regex_SymbolSet.range")
{
    SymbolSet r;
    r.insert(make_pair('a', 'f'));

    CHECK(6 == r.size());
    CHECK("a-f" == r.to_string());

    r.insert(make_pair('0', '9'));
    CHECK(16 == r.size());
    CHECK("0-9a-f" == r.to_string());
}

TEST_CASE("regex_SymbolSet.fmt_format")
{
    SymbolSet s;
    s.insert(make_pair('0', '9'));
    s.insert(make_pair('a', 'f'));

    CHECK("0-9a-f" == fmt::format("{}", s));
}

TEST_CASE("regex_SymbolSet.hash_map")
{
    SymbolSet s0;
    SymbolSet s1 { 'a' };
    SymbolSet s2 { 'a', 'b' };

    unordered_map<SymbolSet, int> map;
    map[s0] = 0;
    map[s1] = 1;
    map[s2] = 2;

    CHECK(0 == map[s0]);
    CHECK(1 == map[s1]);
    CHECK(2 == map[s2]);
}

TEST_CASE("regex_SymbolSet.compare")
{
    SymbolSet s1 { 'a', 'b' };
    SymbolSet s2 { 'a', 'b' };
    SymbolSet s3 { 'a', 'c' };
    REQUIRE(s1 == s2);
    REQUIRE(s1 != s3);
}
