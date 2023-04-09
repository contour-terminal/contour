// This file is part of the "x0" project, http://github.com/christianparpart/x0>
//   (c) 2009-2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Symbols.h>

#include <klex/util/testing.h>

using namespace std;
using regex_dfa::SymbolSet;

TEST(regex_SymbolSet, s0)
{
    SymbolSet s0;
    ASSERT_EQ(0, s0.size());
    ASSERT_TRUE(s0.empty());
}

TEST(regex_SymbolSet, s1)
{
    SymbolSet s1;

    // first add
    s1.insert('a');
    ASSERT_EQ(1, s1.size());
    ASSERT_FALSE(s1.empty());

    // overwrite
    s1.insert('a');
    ASSERT_EQ(1, s1.size());
    ASSERT_FALSE(s1.empty());
}

TEST(regex_SymbolSet, initializer_list)
{
    SymbolSet a { 'a' };
    EXPECT_EQ(1, a.size());
    EXPECT_TRUE(a.contains('a'));

    SymbolSet s2 { 'a', 'b', 'b', 'c' };
    EXPECT_EQ(3, s2.size());
    EXPECT_EQ("abc", s2.to_string());
}

TEST(regex_SymbolSet, dot)
{
    SymbolSet dot(SymbolSet::Dot);
    EXPECT_FALSE(dot.contains('\n'));
    EXPECT_TRUE(dot.contains('\0'));
    EXPECT_TRUE(dot.contains(' '));
    EXPECT_TRUE(dot.isDot());
    EXPECT_EQ(".", dot.to_string());
}

TEST(regex_SymbolSet, complement)
{
    SymbolSet s;
    s.insert('\n');
    EXPECT_EQ("\\n", s.to_string());
    s.complement();
    EXPECT_EQ(".", s.to_string());
}

TEST(regex_SymbolSet, range)
{
    SymbolSet r;
    r.insert(make_pair('a', 'f'));

    EXPECT_EQ(6, r.size());
    EXPECT_EQ("a-f", r.to_string());

    r.insert(make_pair('0', '9'));
    EXPECT_EQ(16, r.size());
    EXPECT_EQ("0-9a-f", r.to_string());
}

TEST(regex_SymbolSet, fmt_format)
{
    SymbolSet s;
    s.insert(make_pair('0', '9'));
    s.insert(make_pair('a', 'f'));

    EXPECT_EQ("0-9a-f", fmt::format("{}", s));
}

TEST(regex_SymbolSet, hash_map)
{
    SymbolSet s0;
    SymbolSet s1 { 'a' };
    SymbolSet s2 { 'a', 'b' };

    unordered_map<SymbolSet, int> map;
    map[s0] = 0;
    map[s1] = 1;
    map[s2] = 2;

    EXPECT_EQ(0, map[s0]);
    EXPECT_EQ(1, map[s1]);
    EXPECT_EQ(2, map[s2]);
}

TEST(regex_SymbolSet, compare)
{
    SymbolSet s1 { 'a', 'b' };
    SymbolSet s2 { 'a', 'b' };
    SymbolSet s3 { 'a', 'c' };
    ASSERT_TRUE(s1 == s2);
    ASSERT_TRUE(s1 != s3);
}
