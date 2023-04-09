// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Compiler.h>
#include <regex_dfa/DFA.h>
#include <regex_dfa/DotWriter.h>
#include <regex_dfa/Lexable.h>
#include <regex_dfa/MultiDFA.h>

#include <klex/util/literals.h>
#include <klex/util/testing.h>

using namespace std;
using namespace regex_dfa;
using namespace regex_dfa::util::literals;

/* FEATURE UNITTEST CHECKLIST:
 *
 * - [ ] concatenation
 * - [ ] alternation
 * - [ ] {n}
 * - [ ] {m,n}
 * - [ ] {m,}
 * - [ ] ?
 * - [ ] character class, [a-z], [a-z0-9]
 * - [ ] character class by name, such as [[:upper:]]
 * - [ ] inverted character class, [^a-z], [^a-z0-9]
 * - [ ] generic lookahead r/s
 * - [ ] EOL lookahead r$
 * - [ ] BOL lookbehind ^r
 */

const string RULES = R"(
  Space(ignore) ::= [\s\t\n]+
  Eof           ::= <<EOF>>
  ABBA          ::= abba
  AB_CD         ::= ab/cd
  CD            ::= cd
  CDEF          ::= cdef
  EOL_LF        ::= eol$
  XAnyLine      ::= x.*
)";

enum class LookaheadToken
{
    Eof = 1,
    ABBA,
    AB_CD,
    CD,
    CDEF,
    EOL_LF,
    XAnyLine
};
namespace fmt
{ // it sucks that I've to specify that here
template <>
struct formatter<LookaheadToken>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const LookaheadToken& v, FormatContext& ctx)
    {
        switch (v)
        {
            case LookaheadToken::Eof: return format_to(ctx.out(), "Eof");
            case LookaheadToken::ABBA: return format_to(ctx.out(), "abba");
            case LookaheadToken::AB_CD: return format_to(ctx.out(), "ab/cd");
            case LookaheadToken::CD: return format_to(ctx.out(), "cd");
            case LookaheadToken::CDEF: return format_to(ctx.out(), "cdef");
            case LookaheadToken::EOL_LF: return format_to(ctx.out(), "eol$");
            case LookaheadToken::XAnyLine: return format_to(ctx.out(), "<XAnyLine>");
            default: return format_to(ctx.out(), "<{}>", static_cast<unsigned>(v));
        }
    }
};
} // namespace fmt

TEST(regex_Lexer, lookahead)
{
    Compiler cc;
    cc.parse(RULES);

    const LexerDef lexerDef = cc.compile();
    logf("LexerDef:\n{}", lexerDef.to_string());
    Lexable<LookaheadToken, StateId, false, true> ls { lexerDef, "abba abcdef", [this](const string& msg) {
                                                          log(msg);
                                                      } };
    auto lexer = begin(ls);

    ASSERT_EQ(LookaheadToken::ABBA, *lexer);
    ASSERT_EQ(LookaheadToken::AB_CD, *++lexer);
    ASSERT_EQ(LookaheadToken::CDEF, *++lexer);
    ASSERT_EQ(LookaheadToken::Eof, *++lexer);
    ASSERT_EQ(end(ls), ++lexer);
}

TEST(regex_Lexable, one)
{
    Compiler cc;
    cc.parse(RULES);

    const LexerDef ld = cc.compile();
    logf("LexerDef:\n{}", ld.to_string());
    auto src = Lexable<LookaheadToken, StateId, false, true> { ld,
                                                               make_unique<stringstream>("abba abcdef"),
                                                               [this](const string& msg) {
                                                                   log(msg);
                                                               } };
    auto lexer = begin(src);
    auto eof = end(src);

    ASSERT_TRUE(lexer != eof);
    EXPECT_EQ(LookaheadToken::ABBA, token(lexer));
    EXPECT_EQ(0, offset(lexer));

    ++lexer;
    EXPECT_EQ(LookaheadToken::AB_CD, token(lexer));
    EXPECT_EQ(5, offset(lexer));

    ++lexer;
    EXPECT_EQ(LookaheadToken::CDEF, token(lexer));
    EXPECT_EQ(7, offset(lexer));

    ++lexer;
    EXPECT_EQ(LookaheadToken::Eof, token(lexer));
    EXPECT_EQ(11, offset(lexer));

    ++lexer;
    ASSERT_FALSE(lexer != eof); // TODO: make that work
}

TEST(regex_Lexer, LexerError)
{
    Compiler cc;
    cc.parse(RULES);

    const LexerDef ld = cc.compile();
    Lexable<LookaheadToken, StateId, false, false> ls { ld, "invalid" };
    EXPECT_THROW(begin(ls), LexerError);
}

TEST(regex_Lexer, evaluateDotToken)
{
    Compiler cc;
    cc.parse(RULES);

    const LexerDef ld = cc.compile();
    Lexable<LookaheadToken, StateId, false, false> ls { ld, "xanything" };
    auto lexer = begin(ls);

    ASSERT_EQ(LookaheadToken::XAnyLine, *lexer);
    ASSERT_EQ(LookaheadToken::Eof, *++lexer);
}

TEST(regex_Lexer, match_eol)
{
    Compiler cc;
    cc.parse(RULES);

    LexerDef ld = cc.compile();
    Lexable<LookaheadToken, StateId, false, true> ls { ld, "abba eol\nabba", [this](const string& msg) {
                                                          log(msg);
                                                      } };
    auto lexer = begin(ls);

    ASSERT_EQ(LookaheadToken::ABBA, *lexer);
    EXPECT_EQ(0, offset(lexer));

    ASSERT_EQ(LookaheadToken::EOL_LF, *++lexer);
    EXPECT_EQ(5, offset(lexer));

    ASSERT_EQ(LookaheadToken::ABBA, *++lexer);
    EXPECT_EQ(9, offset(lexer));

    ASSERT_EQ(LookaheadToken::Eof, *++lexer);
}

TEST(regex_Lexer, bol)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
				|Pragma           ::= ^pragma
				|Test             ::= test
				|Unknown          ::= .
				|Eof              ::= <<EOF>>
				|)"_multiline);

    LexerDef ld = cc.compileMulti();
    Lexable<Tag, StateId, true, true> ls { ld, "pragma", [this](const string& msg) {
                                              log(msg);
                                          } };
    auto lexer = begin(ls);
    ASSERT_EQ(1, *lexer);   // ^pragma
    ASSERT_EQ(4, *++lexer); // EOS
}

TEST(regex_Lexer, bol_no_match)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
			    |Pragma           ::= ^pragma
			    |Test             ::= test
			    |Unknown          ::= .
			    |Eof              ::= <<EOF>>
			    |)"_multiline);

    LexerDef ld = cc.compileMulti();
    logf("LexerDef:\n{}", ld.to_string());
    Lexable<Tag, StateId, true, true> ls { ld, "test pragma", [this](const string& msg) {
                                              log(msg);
                                          } };
    auto lexer = begin(ls);
    ASSERT_EQ(2, *lexer); // test

    // pragma (char-wise) - must not be recognized as ^pragma
    ASSERT_EQ(3, *++lexer);
    ASSERT_EQ(3, *++lexer);
    ASSERT_EQ(3, *++lexer);
    ASSERT_EQ(3, *++lexer);
    ASSERT_EQ(3, *++lexer);
    ASSERT_EQ(3, *++lexer);

    ASSERT_EQ(4, *++lexer); // EOS
}

TEST(regex_Lexer, bol_line2)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
			    |Pragma           ::= ^pragma
			    |Test             ::= test
			    |Eof              ::= <<EOF>>
			    |)"_multiline);

    LexerDef ld = cc.compileMulti();
    logf("LexerDef:\n{}", ld.to_string());
    Lexable<Tag, StateId, true, true> ls { ld, "test\npragma", [this](const string& msg) {
                                              log(msg);
                                          } };
    auto lexer = begin(ls);
    ASSERT_EQ(2, *lexer);   // test
    ASSERT_EQ(1, *++lexer); // ^pragma
    ASSERT_EQ(3, *++lexer); // EOS
}

TEST(regex_Lexer, bol_and_other_conditions)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
			    |Pragma           ::= ^pragma
			    |Test             ::= test
			    |Eof              ::= <<EOF>>
			    |<Asm>Jump        ::= jmp)"_multiline);
    LexerDef ld = cc.compileMulti();
    logf("LexerDef:\n{}", ld.to_string());

    Lexable<Tag, StateId, true, true> ls { ld, "pragma test", [this](const string& msg) {
                                              log(msg);
                                          } };
    auto lexer = begin(ls);
    ASSERT_EQ(1, *lexer);   // ^pragma
    ASSERT_EQ(2, *++lexer); // test
    ASSERT_EQ(3, *++lexer); // <<EOF>>
}

TEST(regex_Lexer, bol_rules_on_non_bol_lexer)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
			    |Eof              ::= <<EOF>>
			    |Test             ::= "test"
			    |Pragma           ::= ^"pragma"
			    |Unknown          ::= .
			    |)"_multiline);

    LexerDef ld = cc.compile();
    using SimpleLexer = Lexable<Tag, StateId, false, false>;
    ASSERT_THROW(SimpleLexer(ld, "pragma"), invalid_argument);
}

TEST(regex_Lexer, non_bol_rules_on_non_bol_lexer)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
			    |Eof              ::= <<EOF>>
			    |Test             ::= "test"
			    |Unknown          ::= .
			    |)"_multiline);

    LexerDef ld = cc.compile();
    Lexable<Tag, StateId, false, false> ls { ld, " test " };
    auto lexer = begin(ls);

    ASSERT_EQ(2, *lexer);   // "test"
    ASSERT_EQ(1, *++lexer); // <<EOF>>
}

TEST(regex_Lexer, non_bol_rules_on_bol_lexer)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
			    |Eof              ::= <<EOF>>
			    |Test             ::= "test"
			    |Unknown          ::= .
			    |)"_multiline);

    LexerDef ld = cc.compile();
    Lexable<Tag, StateId, false, false> ls { ld, " test " };
    auto lexer = begin(ls);

    ASSERT_EQ(2, *lexer);   // "test"
    ASSERT_EQ(1, *++lexer); // <<EOF>>
}

TEST(regex_Lexer, iterator)
{
    Compiler cc;
    cc.parse(make_unique<stringstream>(R"(
		Spacing(ignore) ::= [\s\t\n]+
		A               ::= a
		B               ::= b
		Eof             ::= <<EOF>>
	)"));

    auto const ld = cc.compile();
    auto const ls = Lexable<Tag> { ld, make_unique<stringstream>("a b b a") };
    auto const e = ls.end();
    auto i = ls.begin();

    // a
    ASSERT_EQ(1, *i);
    ASSERT_TRUE(i != e);

    // b
    i++;
    ASSERT_EQ(2, *i);
    ASSERT_TRUE(i != e);

    // b
    i++;
    ASSERT_EQ(2, *i);
    ASSERT_TRUE(i != e);

    // a
    i++;
    ASSERT_EQ(1, *i);
    ASSERT_TRUE(i != e);

    // <<EOF>>
    i++;
    ASSERT_EQ(3, *i);
    ASSERT_TRUE(i != e);

    i++;
    ASSERT_EQ(3, *i); // still EOF
    ASSERT_TRUE(i == e);
}

TEST(regex_Lexer, empty_alt)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore) ::= [\s\t\n]+
			    |Test            ::= aa(bb|)
			    |Eof             ::= <<EOF>>
			    |)"_multiline);

    LexerDef ld = cc.compileMulti();
    Lexable<Tag, StateId, false, true> ls { ld, "aabb aa aabb", [this](const string& msg) {
                                               log(msg);
                                           } };
    auto lexer = begin(ls);

    ASSERT_EQ(1, *lexer);
    ASSERT_EQ(1, *++lexer);
    ASSERT_EQ(1, *++lexer);
    ASSERT_EQ(2, *++lexer); // EOF
}

TEST(regex_Lexer, ignore_many)
{
    Compiler cc;
    cc.parse(R"(|Spacing(ignore)  ::= [\s\t\n]+
			    |Comment(ignore)  ::= #.*
			    |Eof              ::= <<EOF>>
			    |Foo              ::= foo
			    |Bar              ::= bar
			    |)"_multiline);

    LexerDef ld = cc.compileMulti();
    Lexable<int, StateId, false, true> ls { ld,
                                            R"(|# some foo
                                              |foo
                                              |
                                              |# some bar
                                              |bar
                                              |)"_multiline,
                                            [this](const string& msg) {
                                                log(msg);
                                            } };
    auto lexer = begin(ls);

    ASSERT_EQ(2, *lexer);
    ASSERT_EQ("foo", literal(lexer));

    ASSERT_EQ(3, *++lexer);
    ASSERT_EQ("bar", literal(lexer));

    ASSERT_EQ(1, *++lexer); // EOF
}

TEST(regex_Lexer, realworld_ipv4)
{
    Compiler cc;
    cc.parse(R"(|
			    |Spacing(ignore)   ::= [\s\t\n]+
			    |Eof               ::= <<EOF>>
			    |IPv4Octet(ref)    ::= [0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5]
			    |IPv4(ref)         ::= {IPv4Octet}(\.{IPv4Octet}){3}
			    |IPv4Literal       ::= {IPv4}
			    |)"_multiline);

    auto ld = cc.compile();
    auto ls = Lexable<int, StateId, false, true> { ld,
                                                   R"(0.0.0.0 4.2.2.1 10.10.40.199 255.255.255.255)",
                                                   [this](const string& msg) {
                                                       log(msg);
                                                   } };
    auto lexer = begin(ls);

    ASSERT_EQ(2, *lexer);
    ASSERT_EQ("0.0.0.0", literal(lexer));

    ASSERT_EQ(2, *++lexer);
    ASSERT_EQ("4.2.2.1", literal(lexer));

    ASSERT_EQ(2, *++lexer);
    ASSERT_EQ("10.10.40.199", literal(lexer));

    ASSERT_EQ(2, *++lexer);
    ASSERT_EQ("255.255.255.255", literal(lexer));

    ASSERT_EQ(1, *++lexer);
}

enum class RealWorld
{
    Eof = 1,
    IPv4,
    IPv6
};
namespace fmt
{ // it sucks that I've to specify that here
template <>
struct formatter<RealWorld>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const RealWorld& v, FormatContext& ctx)
    {
        switch (v)
        {
            case RealWorld::Eof: return format_to(ctx.out(), "Eof");
            case RealWorld::IPv4: return format_to(ctx.out(), "IPv4");
            case RealWorld::IPv6: return format_to(ctx.out(), "IPv6");
            default: return format_to(ctx.out(), "<{}>", static_cast<unsigned>(v));
        }
    }
};
} // namespace fmt

TEST(regex_Lexer, realworld_ipv6)
{
    Compiler cc;
    cc.parse(R"(|
      |Spacing(ignore)   ::= [\s\t\n]+
      |Eof               ::= <<EOF>>
      |
      |IPv4Octet(ref)    ::= [0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5]
      |IPv4(ref)         ::= {IPv4Octet}(\.{IPv4Octet}){3}
      |IPv4Literal       ::= {IPv4}
      |
      |ipv6Part(ref)     ::= [[:xdigit:]]{1,4}
      |IPv6              ::= {ipv6Part}(:{ipv6Part}){7,7}
      |                    | ({ipv6Part}:){1,7}:
      |                    | :(:{ipv6Part}){1,7}
      |                    | ::
      |                    | ({ipv6Part}:){1}(:{ipv6Part}){0,6}
      |                    | ({ipv6Part}:){2}(:{ipv6Part}){0,5}
      |                    | ({ipv6Part}:){3}(:{ipv6Part}){0,4}
      |                    | ({ipv6Part}:){4}(:{ipv6Part}){0,3}
      |                    | ({ipv6Part}:){5}(:{ipv6Part}){0,2}
      |                    | ({ipv6Part}:){6}(:{ipv6Part}){0,1}
      |                    | ::[fF]{4}:{IPv4}
  )"_multiline);

    static const string TEXT = R"(|0:0:0:0:0:0:0:0
								  |1234:5678:90ab:cdef:aaaa:bbbb:cccc:dddd
								  |2001:0db8:85a3:0000:0000:8a2e:0370:7334
								  |1234:5678::
								  |0::
								  |::0
								  |::
								  |1::3:4:5:6:7:8
								  |1::4:5:6:7:8
								  |1::5:6:7:8
								  |1::8
								  |1:2::4:5:6:7:8
								  |1:2::5:6:7:8
								  |1:2::8
								  |::ffff:127.0.0.1
								  |::ffff:c000:0280
								  |)"_multiline;

    auto ld = cc.compileMulti();
    auto ls = Lexable<RealWorld, StateId, false, true> { ld, TEXT, [this](const string& msg) {
                                                            log(msg);
                                                        } };
    auto lexer = begin(ls);

    ASSERT_EQ(RealWorld::IPv6, *lexer);
    ASSERT_EQ("0:0:0:0:0:0:0:0", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1234:5678:90ab:cdef:aaaa:bbbb:cccc:dddd", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("2001:0db8:85a3:0000:0000:8a2e:0370:7334", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1234:5678::", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("0::", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("::0", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("::", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1::3:4:5:6:7:8", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1::4:5:6:7:8", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1::5:6:7:8", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1::8", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1:2::4:5:6:7:8", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1:2::5:6:7:8", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("1:2::8", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("::ffff:127.0.0.1", literal(lexer));

    ASSERT_EQ(RealWorld::IPv6, *++lexer);
    ASSERT_EQ("::ffff:c000:0280", literal(lexer));

    ASSERT_EQ(RealWorld::Eof, *++lexer);
}

TEST(regex_Lexer, internal)
{
    ASSERT_EQ("Eof", fmt::format("{}", LookaheadToken::Eof));
    ASSERT_EQ("abba", fmt::format("{}", LookaheadToken::ABBA));
    ASSERT_EQ("ab/cd", fmt::format("{}", LookaheadToken::AB_CD));
    ASSERT_EQ("cd", fmt::format("{}", LookaheadToken::CD));
    ASSERT_EQ("cdef", fmt::format("{}", LookaheadToken::CDEF));
    ASSERT_EQ("eol$", fmt::format("{}", LookaheadToken::EOL_LF));
    ASSERT_EQ("<XAnyLine>", fmt::format("{}", LookaheadToken::XAnyLine));
    ASSERT_EQ("<724>", fmt::format("{}", static_cast<LookaheadToken>(724)));

    ASSERT_EQ("Eof", fmt::format("{}", RealWorld::Eof));
    ASSERT_EQ("IPv4", fmt::format("{}", RealWorld::IPv4));
    ASSERT_EQ("IPv6", fmt::format("{}", RealWorld::IPv6));
    ASSERT_EQ("<724>", fmt::format("{}", static_cast<RealWorld>(724)));
}
