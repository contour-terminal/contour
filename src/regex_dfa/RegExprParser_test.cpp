// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/RegExpr.h>
#include <regex_dfa/RegExprParser.h>

#include <memory>

#include <klex/util/testing.h>

using namespace std;
using namespace regex_dfa;

TEST(regex_RegExprParser, namedCharacterClass_graph)
{
    RegExpr re = RegExprParser {}.parse("[[:graph:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("!-~", to_string(re));
}

TEST(regex_RegExprParser, whitespaces_concatination)
{
    RegExpr re = RegExprParser {}.parse("a b");
    ASSERT_TRUE(holds_alternative<ConcatenationExpr>(re));
    EXPECT_EQ("ab", to_string(re));
}

TEST(regex_RegExprParser, whitespaces_alternation)
{
    RegExpr re = RegExprParser {}.parse("a | b");
    ASSERT_TRUE(holds_alternative<ConcatenationExpr>(re));
    EXPECT_EQ("a|b", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_digit)
{
    RegExpr re = RegExprParser {}.parse("[[:digit:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("0-9", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_alnum)
{
    RegExpr re = RegExprParser {}.parse("[[:alnum:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("0-9A-Za-z", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_alpha)
{
    RegExpr re = RegExprParser {}.parse("[[:alpha:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("A-Za-z", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_blank)
{
    RegExpr re = RegExprParser {}.parse("[[:blank:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("\\t\\s", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_cntrl)
{
    RegExpr re = RegExprParser {}.parse("[[:cntrl:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("\\0-\\x1f\\x7f", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_print)
{
    RegExpr re = RegExprParser {}.parse("[[:print:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("\\s-~", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_punct)
{
    RegExpr re = RegExprParser {}.parse("[[:punct:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("!-/:-@[-`{-~", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_space)
{
    RegExpr re = RegExprParser {}.parse("[[:space:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("\\0\\t-\\r", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_unknown)
{
    EXPECT_THROW(RegExprParser {}.parse("[[:unknown:]]"), RegExprParser::UnexpectedToken);
}

TEST(regex_RegExprParser, namedCharacterClass_upper)
{
    RegExpr re = RegExprParser {}.parse("[[:upper:]]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("A-Z", to_string(re));
}

TEST(regex_RegExprParser, namedCharacterClass_mixed)
{
    RegExpr re = RegExprParser {}.parse("[[:lower:]0-9]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("0-9a-z", to_string(re));
}

TEST(regex_RegExprParser, characterClass_complement)
{
    RegExpr re = RegExprParser {}.parse("[^\\n]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_TRUE(get<CharacterClassExpr>(re).symbols.isDot());
    EXPECT_EQ(".", get<CharacterClassExpr>(re).symbols.to_string());
}

TEST(regex_RegExprParser, escapeSequences_invalid)
{
    EXPECT_THROW(RegExprParser {}.parse("[\\z]"), RegExprParser::UnexpectedToken);
}

TEST(regex_RegExprParser, escapeSequences_abfnrstv)
{
    EXPECT_EQ("\\a", to_string(RegExprParser {}.parse("[\\a]")));
    EXPECT_EQ("\\b", to_string(RegExprParser {}.parse("[\\b]")));
    EXPECT_EQ("\\f", to_string(RegExprParser {}.parse("[\\f]")));
    EXPECT_EQ("\\n", to_string(RegExprParser {}.parse("[\\n]")));
    EXPECT_EQ("\\r", to_string(RegExprParser {}.parse("[\\r]")));
    EXPECT_EQ("\\s", to_string(RegExprParser {}.parse("[\\s]")));
    EXPECT_EQ("\\t", to_string(RegExprParser {}.parse("[\\t]")));
    EXPECT_EQ("\\v", to_string(RegExprParser {}.parse("[\\v]")));
}

TEST(regex_RegExprParser, newline)
{
    RegExpr re = RegExprParser {}.parse("\n");
    ASSERT_TRUE(holds_alternative<CharacterExpr>(re));
    EXPECT_EQ('\n', get<CharacterExpr>(re).value);
}

TEST(regex_RegExprParser, escapeSequences_hex)
{
    RegExpr re = RegExprParser {}.parse("[\\x20]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("\\s", get<CharacterClassExpr>(re).symbols.to_string());

    EXPECT_THROW(RegExprParser {}.parse("[\\xZZ]"), RegExprParser::UnexpectedToken);
    EXPECT_THROW(RegExprParser {}.parse("[\\xAZ]"), RegExprParser::UnexpectedToken);
    EXPECT_THROW(RegExprParser {}.parse("[\\xZA]"), RegExprParser::UnexpectedToken);
}

TEST(regex_RegExprParser, escapeSequences_nul)
{
    RegExpr re = RegExprParser {}.parse("[\\0]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("\\0", get<CharacterClassExpr>(re).symbols.to_string());
}

TEST(regex_RegExprParser, escapeSequences_octal)
{
    // with leading zero
    RegExpr re = RegExprParser {}.parse("[\\040]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("\\s", get<CharacterClassExpr>(re).symbols.to_string());

    // with leading non-zero
    re = RegExprParser {}.parse("[\\172]");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ("z", get<CharacterClassExpr>(re).symbols.to_string());

    // invalids
    EXPECT_THROW(RegExprParser {}.parse("[\\822]"), RegExprParser::UnexpectedToken);
    EXPECT_THROW(RegExprParser {}.parse("[\\282]"), RegExprParser::UnexpectedToken);
    EXPECT_THROW(RegExprParser {}.parse("[\\228]"), RegExprParser::UnexpectedToken);
    EXPECT_THROW(RegExprParser {}.parse("[\\082]"), RegExprParser::UnexpectedToken);
    EXPECT_THROW(RegExprParser {}.parse("[\\028]"), RegExprParser::UnexpectedToken);
}

TEST(regex_RegExprParser, doubleQuote)
{
    // as concatenation character
    RegExpr re = RegExprParser {}.parse(R"(\")");
    ASSERT_TRUE(holds_alternative<CharacterExpr>(re));
    EXPECT_EQ('"', get<CharacterExpr>(re).value);

    // as character class
    re = RegExprParser {}.parse(R"([\"])");
    ASSERT_TRUE(holds_alternative<CharacterClassExpr>(re));
    EXPECT_EQ(R"(")", get<CharacterClassExpr>(re).symbols.to_string());
}

TEST(regex_RegExprParser, dot)
{
    RegExpr re = RegExprParser {}.parse(".");
    ASSERT_TRUE(holds_alternative<DotExpr>(re));
    EXPECT_EQ(".", to_string(re));
}

TEST(regex_RegExprParser, optional)
{
    RegExpr re = RegExprParser {}.parse("a?");
    ASSERT_TRUE(holds_alternative<ClosureExpr>(re));
    EXPECT_EQ("a?", to_string(re));
}

TEST(regex_RegExprParser, bol)
{
    RegExpr re = RegExprParser {}.parse("^a");
    ASSERT_TRUE(holds_alternative<ConcatenationExpr>(re));
    const ConcatenationExpr& cat = get<ConcatenationExpr>(re);

    ASSERT_TRUE(holds_alternative<BeginOfLineExpr>(*cat.left));
    EXPECT_EQ("^", to_string(*cat.left));
    EXPECT_EQ("a", to_string(*cat.right));
}

TEST(regex_RegExprParser, eol)
{
    RegExpr re = RegExprParser {}.parse("a$");
    ASSERT_TRUE(holds_alternative<ConcatenationExpr>(re));
    const ConcatenationExpr& cat = get<ConcatenationExpr>(re);

    ASSERT_TRUE(holds_alternative<EndOfLineExpr>(*cat.right));
    EXPECT_EQ("a$", to_string(re));
}

TEST(regex_RegExprParser, eof)
{
    RegExpr re = RegExprParser {}.parse("<<EOF>>");
    ASSERT_TRUE(holds_alternative<EndOfFileExpr>(re));
    EXPECT_EQ("<<EOF>>", to_string(re));
}

TEST(regex_RegExprParser, alternation)
{
    EXPECT_EQ("a|b", to_string(RegExprParser {}.parse("a|b")));
    EXPECT_EQ("(a|b)c", to_string(RegExprParser {}.parse("(a|b)c")));
    EXPECT_EQ("a(b|c)", to_string(RegExprParser {}.parse("a(b|c)")));
}

TEST(regex_RegExprParser, lookahead)
{
    RegExpr re = RegExprParser {}.parse("ab/cd");
    ASSERT_TRUE(holds_alternative<LookAheadExpr>(re));
    EXPECT_EQ("ab/cd", to_string(re));
    EXPECT_EQ("(a/b)|b", to_string(RegExprParser {}.parse("(a/b)|b")));
    EXPECT_EQ("a|(b/c)", to_string(RegExprParser {}.parse("a|(b/c)")));
}

TEST(regex_RegExprParser, closure)
{
    RegExpr re = RegExprParser {}.parse("(abc)*");
    ASSERT_TRUE(holds_alternative<ClosureExpr>(re));
    const ClosureExpr& e = get<ClosureExpr>(re);
    EXPECT_EQ(0, e.minimumOccurrences);
    EXPECT_EQ(numeric_limits<unsigned>::max(), e.maximumOccurrences);
    EXPECT_EQ("(abc)*", to_string(re));
}

TEST(regex_RegExprParser, positive)
{
    auto re = RegExprParser {}.parse("(abc)+");
    ASSERT_TRUE(holds_alternative<ClosureExpr>(re));
    const ClosureExpr& e = get<ClosureExpr>(re);
    EXPECT_EQ(1, e.minimumOccurrences);
    EXPECT_EQ(numeric_limits<unsigned>::max(), e.maximumOccurrences);
    EXPECT_EQ("(abc)+", to_string(re));
}

TEST(regex_RegExprParser, closure_range)
{
    auto re = RegExprParser {}.parse("a{2,4}");
    ASSERT_TRUE(holds_alternative<ClosureExpr>(re));
    const ClosureExpr& e = get<ClosureExpr>(re);
    EXPECT_EQ(2, e.minimumOccurrences);
    EXPECT_EQ(4, e.maximumOccurrences);
    EXPECT_EQ("a{2,4}", to_string(re));
}

TEST(regex_RegExprParser, empty)
{
    auto re = RegExprParser {}.parse("(a|)");
    EXPECT_EQ("a|", to_string(re)); // grouping '(' & ')' is not preserved as node in the parse tree.
}

TEST(regex_RegExprParser, UnexpectedToken_grouping)
{
    EXPECT_THROW(RegExprParser {}.parse("(a"), RegExprParser::UnexpectedToken);
}

TEST(regex_RegExprParser, UnexpectedToken_literal)
{
    EXPECT_THROW(RegExprParser {}.parse("\"a"), RegExprParser::UnexpectedToken);
}
