// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/RuleParser.h>

#include <catch2/catch.hpp>

#include <memory>
#include <sstream>

using namespace regex_dfa;

TEST_CASE("regex_RuleParser.simple")
{
    RuleParser rp { "main ::= blah\n" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK("blah" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.whitespaces")
{
    RuleParser rp { "main ::= a\n\t| b | c\n" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK("a|b | c" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.rule_at_eof")
{
    RuleParser rp { "main ::= blah" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK("blah" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.simple_trailing_spaces")
{
    RuleParser rp { "main ::= blah\n   " };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK("blah" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.quotedPattern")
{
    RuleParser rp { "main ::= \"blah\"" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK("\"blah\"" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.multiQuotedPattern")
{
    RuleParser rp { R"(rule ::= "b"la"h")" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK(R"("b"la"h")" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.doubleQuote")
{
    RuleParser rp { R"(rule ::= \")" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK(R"(\")" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.spaceRule")
{
    RuleParser rp { R"(rule ::= [ \n\t]+)" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK(R"([ \n\t]+)" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.stringRule")
{
    RuleParser rp { R"(rule ::= \"[^\"]*\")" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK(R"(\"[^\"]*\")" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.ref")
{
    RuleParser rp { R"(
    Foo(ref) ::= foo
    Bar(ref) ::= bar
    FooBar   ::= {Foo}_{Bar}
  )" };
    RuleList rules = rp.parseRules();
    REQUIRE(1 == rules.size());
    CHECK("(foo)_(bar)" == rules[0].pattern);
}

TEST_CASE("regex_RuleParser.ref_duplicated")
{
    RuleParser rp { R"(
    Foo(ref) ::= foo
    Foo(ref) ::= bar
    FooBar   ::= {Foo}
  )" };
    CHECK_THROWS_AS(rp.parseRules(), RuleParser::DuplicateRule);
}

TEST_CASE("regex_RuleParser.multiline_alt")
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
    REQUIRE(2 == rules.size());
    CHECK("foo|bar" == rules[0].pattern);
    CHECK("(fnord|hard)|(fnord|hard)" == rules[1].pattern);
}

TEST_CASE("regex_RuleParser.condition1")
{
    RuleParser rp { R"(
    <foo>Rule1    ::= foo
    <bar>Rule2    ::= bar
  )" };
    RuleList rules = rp.parseRules();

    REQUIRE(2 == rules.size());
    CHECK("foo" == rules[0].pattern);
    CHECK("bar" == rules[1].pattern);

    REQUIRE(1 == rules[0].conditions.size());
    CHECK("foo" == rules[0].conditions[0]);

    REQUIRE(1 == rules[1].conditions.size());
    CHECK("bar" == rules[1].conditions[0]);
}

TEST_CASE("regex_RuleParser.condition2")
{
    RuleParser rp { R"(
    <foo>Rule1      ::= foo
    <foo,bar>Rule2  ::= bar
  )" };
    RuleList rules = rp.parseRules();

    REQUIRE(2 == rules.size());
    CHECK("foo" == rules[0].pattern);
    CHECK("bar" == rules[1].pattern);

    REQUIRE(1 == rules[0].conditions.size());
    CHECK("foo" == rules[0].conditions[0]);

    REQUIRE(2 == rules[1].conditions.size());
    // in sorted order
    CHECK("bar" == rules[1].conditions[0]);
    CHECK("foo" == rules[1].conditions[1]);
}

TEST_CASE("regex_RuleParser.conditional_star")
{
    RuleParser rp { R"(
    Zero      ::= zero
    <one>One  ::= one
    <two>Two  ::= two
    <*>Tri    ::= tri
  )" };
    RuleList rules = rp.parseRules();

    REQUIRE(4 == rules.size());

    CHECK("zero" == rules[0].pattern);
    REQUIRE(1 == rules[0].conditions.size());
    CHECK("INITIAL" == rules[0].conditions[0]);

    CHECK("one" == rules[1].pattern);
    REQUIRE(1 == rules[1].conditions.size());
    CHECK("one" == rules[1].conditions[0]);

    CHECK("two" == rules[2].pattern);
    REQUIRE(1 == rules[2].conditions.size());
    CHECK("two" == rules[2].conditions[0]);

    CHECK("tri" == rules[3].pattern);
    REQUIRE(3 == rules[3].conditions.size());
    CHECK("INITIAL" == rules[3].conditions[0]);
    CHECK("one" == rules[3].conditions[1]);
    CHECK("two" == rules[3].conditions[2]);
}

TEST_CASE("regex_RuleParser.grouped_conditions")
{
    RuleParser rp { R"(
    Rule1       ::= foo
    <blah> {
      Rule2     ::= bar
    }
  )" };
    RuleList rules = rp.parseRules();

    REQUIRE(2 == rules.size());
    CHECK("foo" == rules[0].pattern);
    CHECK("bar" == rules[1].pattern);

    REQUIRE(1 == rules[1].conditions.size());
    CHECK("blah" == rules[1].conditions[0]);
}

TEST_CASE("regex_RuleParser.InvalidRefRuleWithConditions")
{
    CHECK_THROWS_AS(RuleParser { "<cond>main(ref) ::= blah\n" }.parseRules(),
                    RuleParser::InvalidRefRuleWithConditions);
}

TEST_CASE("regex_RuleParser.InvalidRuleOption")
{
    CHECK_THROWS_AS(RuleParser { "A(invalid) ::= a\n" }.parseRules(), RuleParser::InvalidRuleOption);
}

TEST_CASE("regex_RuleParser.DuplicateRule")
{
    RuleParser rp { R"(
    foo ::= abc
    foo ::= def
  )" };
    CHECK_THROWS_AS(rp.parseRules(), RuleParser::DuplicateRule);
}

TEST_CASE("regex_RuleParser.UnexpectedChar")
{
    CHECK_THROWS_AS(RuleParser { "A :=" }.parseRules(), RuleParser::UnexpectedChar);
    CHECK_THROWS_AS(RuleParser { "<x A ::= a" }.parseRules(), RuleParser::UnexpectedChar);
}

TEST_CASE("regex_RuleParser.UnexpectedToken")
{
    CHECK_THROWS_AS(RuleParser { "<x,y,> A ::= a" }.parseRules(), RuleParser::UnexpectedToken);
    CHECK_THROWS_AS(RuleParser { "<> A ::= a" }.parseRules(), RuleParser::UnexpectedToken);
    CHECK_THROWS_AS(RuleParser { " ::= a" }.parseRules(), RuleParser::UnexpectedToken);
}
