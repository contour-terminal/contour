// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/RegExpr.h>
#include <regex_dfa/RegExprParser.h>

#include <catch2/catch.hpp>

#include <memory>

using namespace std;
using namespace regex_dfa;

namespace
{

RegExpr parseRegExpr(string const& s)
{
    return RegExprParser {}.parse(s);
}

} // namespace

TEST_CASE("regex_RegExprParser.namedCharacterClass_graph")
{
    RegExpr re = parseRegExpr("[[:graph:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("!-~" == to_string(re));
}

TEST_CASE("regex_RegExprParser.whitespaces_concatination")
{
    RegExpr re = parseRegExpr("a b");
    REQUIRE(holds_alternative<ConcatenationExpr>(re));
    CHECK("ab" == to_string(re));
}

TEST_CASE("regex_RegExprParser.whitespaces_alternation")
{
    RegExpr re = parseRegExpr("a | b");
    REQUIRE(holds_alternative<ConcatenationExpr>(re));
    CHECK("a|b" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_digit")
{
    RegExpr re = parseRegExpr("[[:digit:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("0-9" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_alnum")
{
    RegExpr re = parseRegExpr("[[:alnum:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("0-9A-Za-z" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_alpha")
{
    RegExpr re = parseRegExpr("[[:alpha:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("A-Za-z" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_blank")
{
    RegExpr re = parseRegExpr("[[:blank:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("\\t\\s" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_cntrl")
{
    RegExpr re = parseRegExpr("[[:cntrl:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("\\0-\\x1f\\x7f" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_print")
{
    RegExpr re = parseRegExpr("[[:print:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("\\s-~" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_punct")
{
    RegExpr re = parseRegExpr("[[:punct:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("!-/:-@[-`{-~" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_space")
{
    RegExpr re = parseRegExpr("[[:space:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("\\0\\t-\\r" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_unknown")
{
    CHECK_THROWS_AS(parseRegExpr("[[:unknown:]]"), RegExprParser::UnexpectedToken);
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_upper")
{
    RegExpr re = parseRegExpr("[[:upper:]]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("A-Z" == to_string(re));
}

TEST_CASE("regex_RegExprParser.namedCharacterClass_mixed")
{
    RegExpr re = parseRegExpr("[[:lower:]0-9]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("0-9a-z" == to_string(re));
}

TEST_CASE("regex_RegExprParser.characterClass_complement")
{
    RegExpr re = parseRegExpr("[^\\n]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK(get<CharacterClassExpr>(re).symbols.isDot());
    CHECK("." == get<CharacterClassExpr>(re).symbols.to_string());
}

TEST_CASE("regex_RegExprParser.escapeSequences_invalid")
{
    CHECK_THROWS_AS(parseRegExpr("[\\z]"), RegExprParser::UnexpectedToken);
}

TEST_CASE("regex_RegExprParser.escapeSequences_abfnrstv")
{
    CHECK("\\a" == to_string(parseRegExpr("[\\a]")));
    CHECK("\\b" == to_string(parseRegExpr("[\\b]")));
    CHECK("\\f" == to_string(parseRegExpr("[\\f]")));
    CHECK("\\n" == to_string(parseRegExpr("[\\n]")));
    CHECK("\\r" == to_string(parseRegExpr("[\\r]")));
    CHECK("\\s" == to_string(parseRegExpr("[\\s]")));
    CHECK("\\t" == to_string(parseRegExpr("[\\t]")));
    CHECK("\\v" == to_string(parseRegExpr("[\\v]")));
}

TEST_CASE("regex_RegExprParser.newline")
{
    RegExpr re = parseRegExpr("\n");
    REQUIRE(holds_alternative<CharacterExpr>(re));
    CHECK('\n' == get<CharacterExpr>(re).value);
}

TEST_CASE("regex_RegExprParser.escapeSequences_hex")
{
    RegExpr re = parseRegExpr("[\\x20]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("\\s" == get<CharacterClassExpr>(re).symbols.to_string());

    CHECK_THROWS_AS(parseRegExpr("[\\xZZ]"), RegExprParser::UnexpectedToken);
    CHECK_THROWS_AS(parseRegExpr("[\\xAZ]"), RegExprParser::UnexpectedToken);
    CHECK_THROWS_AS(parseRegExpr("[\\xZA]"), RegExprParser::UnexpectedToken);
}

TEST_CASE("regex_RegExprParser.escapeSequences_nul")
{
    RegExpr re = parseRegExpr("[\\0]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("\\0" == get<CharacterClassExpr>(re).symbols.to_string());
}

TEST_CASE("regex_RegExprParser.escapeSequences_octal")
{
    // with leading zero
    RegExpr re = parseRegExpr("[\\040]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("\\s" == get<CharacterClassExpr>(re).symbols.to_string());

    // with leading non-zero
    re = parseRegExpr("[\\172]");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK("z" == get<CharacterClassExpr>(re).symbols.to_string());

    // invalids
    CHECK_THROWS_AS(parseRegExpr("[\\822]"), RegExprParser::UnexpectedToken);
    CHECK_THROWS_AS(parseRegExpr("[\\282]"), RegExprParser::UnexpectedToken);
    CHECK_THROWS_AS(parseRegExpr("[\\228]"), RegExprParser::UnexpectedToken);
    CHECK_THROWS_AS(parseRegExpr("[\\082]"), RegExprParser::UnexpectedToken);
    CHECK_THROWS_AS(parseRegExpr("[\\028]"), RegExprParser::UnexpectedToken);
}

TEST_CASE("regex_RegExprParser.doubleQuote")
{
    // as concatenation character
    RegExpr re = parseRegExpr(R"(\")");
    REQUIRE(holds_alternative<CharacterExpr>(re));
    CHECK('"' == get<CharacterExpr>(re).value);

    // as character class
    re = parseRegExpr(R"([\"])");
    REQUIRE(holds_alternative<CharacterClassExpr>(re));
    CHECK(R"(")" == get<CharacterClassExpr>(re).symbols.to_string());
}

TEST_CASE("regex_RegExprParser.dot")
{
    RegExpr re = parseRegExpr(".");
    REQUIRE(holds_alternative<DotExpr>(re));
    CHECK("." == to_string(re));
}

TEST_CASE("regex_RegExprParser.optional")
{
    RegExpr re = parseRegExpr("a?");
    REQUIRE(holds_alternative<ClosureExpr>(re));
    CHECK("a?" == to_string(re));
}

TEST_CASE("regex_RegExprParser.bol")
{
    RegExpr re = parseRegExpr("^a");
    REQUIRE(holds_alternative<ConcatenationExpr>(re));
    const ConcatenationExpr& cat = get<ConcatenationExpr>(re);

    REQUIRE(holds_alternative<BeginOfLineExpr>(*cat.left));
    CHECK("^" == to_string(*cat.left));
    CHECK("a" == to_string(*cat.right));
}

TEST_CASE("regex_RegExprParser.eol")
{
    RegExpr re = parseRegExpr("a$");
    REQUIRE(holds_alternative<ConcatenationExpr>(re));
    const ConcatenationExpr& cat = get<ConcatenationExpr>(re);

    REQUIRE(holds_alternative<EndOfLineExpr>(*cat.right));
    CHECK("a$" == to_string(re));
}

TEST_CASE("regex_RegExprParser.eof")
{
    RegExpr re = parseRegExpr("<<EOF>>");
    REQUIRE(holds_alternative<EndOfFileExpr>(re));
    CHECK("<<EOF>>" == to_string(re));
}

TEST_CASE("regex_RegExprParser.alternation")
{
    CHECK("a|b" == to_string(parseRegExpr("a|b")));
    CHECK("(a|b)c" == to_string(parseRegExpr("(a|b)c")));
    CHECK("a(b|c)" == to_string(parseRegExpr("a(b|c)")));
}

TEST_CASE("regex_RegExprParser.lookahead")
{
    RegExpr re = parseRegExpr("ab/cd");
    REQUIRE(holds_alternative<LookAheadExpr>(re));
    CHECK("ab/cd" == to_string(re));
    CHECK("(a/b)|b" == to_string(parseRegExpr("(a/b)|b")));
    CHECK("a|(b/c)" == to_string(parseRegExpr("a|(b/c)")));
}

TEST_CASE("regex_RegExprParser.closure")
{
    RegExpr re = parseRegExpr("(abc)*");
    REQUIRE(holds_alternative<ClosureExpr>(re));
    const ClosureExpr& e = get<ClosureExpr>(re);
    CHECK(0 == e.minimumOccurrences);
    CHECK(numeric_limits<unsigned>::max() == e.maximumOccurrences);
    CHECK("(abc)*" == to_string(re));
}

TEST_CASE("regex_RegExprParser.positive")
{
    auto re = parseRegExpr("(abc)+");
    REQUIRE(holds_alternative<ClosureExpr>(re));
    const ClosureExpr& e = get<ClosureExpr>(re);
    CHECK(1 == e.minimumOccurrences);
    CHECK(numeric_limits<unsigned>::max() == e.maximumOccurrences);
    CHECK("(abc)+" == to_string(re));
}

TEST_CASE("regex_RegExprParser.closure_range")
{
    auto re = parseRegExpr("a{2,4}");
    REQUIRE(holds_alternative<ClosureExpr>(re));
    const ClosureExpr& e = get<ClosureExpr>(re);
    CHECK(2 == e.minimumOccurrences);
    CHECK(4 == e.maximumOccurrences);
    CHECK("a{2,4}" == to_string(re));
}

TEST_CASE("regex_RegExprParser.empty")
{
    auto re = parseRegExpr("(a|)");
    CHECK("a|" == to_string(re)); // grouping '(' & ')' is not preserved as node in the parse tree.
}

TEST_CASE("regex_RegExprParser.UnexpectedToken_grouping")
{
    CHECK_THROWS_AS(parseRegExpr("(a"), RegExprParser::UnexpectedToken);
}

TEST_CASE("regex_RegExprParser.UnexpectedToken_literal")
{
    CHECK_THROWS_AS(parseRegExpr("\"a"), RegExprParser::UnexpectedToken);
}
