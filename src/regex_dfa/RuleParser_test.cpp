// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/RuleParser.h>

#include <memory>
#include <sstream>

#include <klex/util/testing.h>

using namespace regex_dfa;

TEST(regex_RuleParser, simple)
{
    RuleParser rp { "main ::= blah\n" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ("blah", rules[0].pattern);
}

TEST(regex_RuleParser, whitespaces)
{
    RuleParser rp { "main ::= a\n\t| b | c\n" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ("a|b | c", rules[0].pattern);
}

TEST(regex_RuleParser, rule_at_eof)
{
    RuleParser rp { "main ::= blah" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ("blah", rules[0].pattern);
}

TEST(regex_RuleParser, simple_trailing_spaces)
{
    RuleParser rp { "main ::= blah\n   " };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ("blah", rules[0].pattern);
}

TEST(regex_RuleParser, quotedPattern)
{
    RuleParser rp { "main ::= \"blah\"" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ("\"blah\"", rules[0].pattern);
}

TEST(regex_RuleParser, multiQuotedPattern)
{
    RuleParser rp { R"(rule ::= "b"la"h")" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ(R"("b"la"h")", rules[0].pattern);
}

TEST(regex_RuleParser, doubleQuote)
{
    RuleParser rp { R"(rule ::= \")" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ(R"(\")", rules[0].pattern);
}

TEST(regex_RuleParser, spaceRule)
{
    RuleParser rp { R"(rule ::= [ \n\t]+)" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ(R"([ \n\t]+)", rules[0].pattern);
}

TEST(regex_RuleParser, stringRule)
{
    RuleParser rp { R"(rule ::= \"[^\"]*\")" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ(R"(\"[^\"]*\")", rules[0].pattern);
}

TEST(regex_RuleParser, ref)
{
    RuleParser rp { R"(
    Foo(ref) ::= foo
    Bar(ref) ::= bar
    FooBar   ::= {Foo}_{Bar}
  )" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(1, rules.size());
    EXPECT_EQ("(foo)_(bar)", rules[0].pattern);
}

TEST(regex_RuleParser, ref_duplicated)
{
    RuleParser rp { R"(
    Foo(ref) ::= foo
    Foo(ref) ::= bar
    FooBar   ::= {Foo}
  )" };
    EXPECT_THROW(rp.parseRules(), RuleParser::DuplicateRule);
}

TEST(regex_RuleParser, multiline_alt)
{
    RuleParser rp { R"(
    Rule1       ::= foo
                  | bar
    Rule2(ref)  ::= fnord
                  | hard
    Rule3       ::= {Rule2}
                  | {Rule2}
  )" };
    RuleList rules = rp.parseRules();
    ASSERT_EQ(2, rules.size());
    EXPECT_EQ("foo|bar", rules[0].pattern);
    EXPECT_EQ("(fnord|hard)|(fnord|hard)", rules[1].pattern);
}

TEST(regex_RuleParser, condition1)
{
    RuleParser rp { R"(
    <foo>Rule1    ::= foo
    <bar>Rule2    ::= bar
  )" };
    RuleList rules = rp.parseRules();

    ASSERT_EQ(2, rules.size());
    EXPECT_EQ("foo", rules[0].pattern);
    EXPECT_EQ("bar", rules[1].pattern);

    ASSERT_EQ(1, rules[0].conditions.size());
    EXPECT_EQ("foo", rules[0].conditions[0]);

    ASSERT_EQ(1, rules[1].conditions.size());
    EXPECT_EQ("bar", rules[1].conditions[0]);
}

TEST(regex_RuleParser, condition2)
{
    RuleParser rp { R"(
    <foo>Rule1      ::= foo
    <foo,bar>Rule2  ::= bar
  )" };
    RuleList rules = rp.parseRules();

    ASSERT_EQ(2, rules.size());
    EXPECT_EQ("foo", rules[0].pattern);
    EXPECT_EQ("bar", rules[1].pattern);

    ASSERT_EQ(1, rules[0].conditions.size());
    EXPECT_EQ("foo", rules[0].conditions[0]);

    ASSERT_EQ(2, rules[1].conditions.size());
    // in sorted order
    EXPECT_EQ("bar", rules[1].conditions[0]);
    EXPECT_EQ("foo", rules[1].conditions[1]);
}

TEST(regex_RuleParser, conditional_star)
{
    RuleParser rp { R"(
    Zero      ::= zero
    <one>One  ::= one
    <two>Two  ::= two
    <*>Tri    ::= tri
  )" };
    RuleList rules = rp.parseRules();

    ASSERT_EQ(4, rules.size());

    EXPECT_EQ("zero", rules[0].pattern);
    ASSERT_EQ(1, rules[0].conditions.size());
    EXPECT_EQ("INITIAL", rules[0].conditions[0]);

    EXPECT_EQ("one", rules[1].pattern);
    ASSERT_EQ(1, rules[1].conditions.size());
    EXPECT_EQ("one", rules[1].conditions[0]);

    EXPECT_EQ("two", rules[2].pattern);
    ASSERT_EQ(1, rules[2].conditions.size());
    EXPECT_EQ("two", rules[2].conditions[0]);

    EXPECT_EQ("tri", rules[3].pattern);
    ASSERT_EQ(3, rules[3].conditions.size());
    EXPECT_EQ("INITIAL", rules[3].conditions[0]);
    EXPECT_EQ("one", rules[3].conditions[1]);
    EXPECT_EQ("two", rules[3].conditions[2]);
}

TEST(regex_RuleParser, grouped_conditions)
{
    RuleParser rp { R"(
    Rule1       ::= foo
    <blah> {
      Rule2     ::= bar
    }
  )" };
    RuleList rules = rp.parseRules();

    ASSERT_EQ(2, rules.size());
    EXPECT_EQ("foo", rules[0].pattern);
    EXPECT_EQ("bar", rules[1].pattern);

    ASSERT_EQ(1, rules[1].conditions.size());
    EXPECT_EQ("blah", rules[1].conditions[0]);
}

TEST(regex_RuleParser, InvalidRefRuleWithConditions)
{
    ASSERT_THROW(RuleParser { "<cond>main(ref) ::= blah\n" }.parseRules(),
                 RuleParser::InvalidRefRuleWithConditions);
}

TEST(regex_RuleParser, InvalidRuleOption)
{
    ASSERT_THROW(RuleParser { "A(invalid) ::= a\n" }.parseRules(), RuleParser::InvalidRuleOption);
}

TEST(regex_RuleParser, DuplicateRule)
{
    RuleParser rp { R"(
    foo ::= abc
    foo ::= def
  )" };
    ASSERT_THROW(rp.parseRules(), RuleParser::DuplicateRule);
}

TEST(regex_RuleParser, UnexpectedChar)
{
    ASSERT_THROW(RuleParser { "A :=" }.parseRules(), RuleParser::UnexpectedChar);
    ASSERT_THROW(RuleParser { "<x A ::= a" }.parseRules(), RuleParser::UnexpectedChar);
}

TEST(regex_RuleParser, UnexpectedToken)
{
    ASSERT_THROW(RuleParser { "<x,y,> A ::= a" }.parseRules(), RuleParser::UnexpectedToken);
    ASSERT_THROW(RuleParser { "<> A ::= a" }.parseRules(), RuleParser::UnexpectedToken);
    ASSERT_THROW(RuleParser { " ::= a" }.parseRules(), RuleParser::UnexpectedToken);
}
