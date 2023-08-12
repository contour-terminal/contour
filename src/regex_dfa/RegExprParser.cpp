// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/RegExpr.h>
#include <regex_dfa/RegExprParser.h>
#include <regex_dfa/Symbols.h>

#include <fmt/format.h>

#include <functional>
#include <iostream>
#include <limits>
#include <sstream>

using namespace std;

#if 0
    #define DEBUG(msg, ...)                                \
        do                                                 \
        {                                                  \
            cerr << fmt::format(msg, __VA_ARGS__) << "\n"; \
        } while (0)
#else
    #define DEBUG(msg, ...) \
        do                  \
        {                   \
        } while (0)
#endif

/*
  REGULAR EXPRESSION SYNTAX:
  --------------------------

  expr                    := alternation
  alternation             := concatenation ('|' concatenation)*
  concatenation           := closure (closure)*
  closure                 := atom ['*' | '?' | '{' NUM [',' NUM] '}']
  atom                    := character
                           | '^'
                           | '$'
                           | '<<EOF>>'
                           | '"' LITERAL '"'
                           | characterClass
                           | '(' expr ')'
                           | EPSILON
  characterClass          := '[' ['^'] characterClassFragment+ ']'
  characterClassFragment  := character | character '-' character
*/

namespace regex_dfa
{

RegExprParser::RegExprParser(): input_ {}, currentChar_ { input_.end() }, line_ { 1 }, column_ { 0 }
{
}

int RegExprParser::currentChar() const
{
    if (currentChar_ != input_.end())
        return *currentChar_;
    else
        return std::char_traits<char>::eof();
}

bool RegExprParser::consumeIf(int ch)
{
    if (currentChar() != ch)
        return false;

    consume();
    return true;
}

int RegExprParser::consume()
{
    if (currentChar_ == input_.end())
        return std::char_traits<char>::eof();

    int ch = *currentChar_;
    if (ch == '\n')
    {
        line_++;
        column_ = 1;
    }
    else
    {
        column_++;
    }
    ++currentChar_;
    DEBUG("consume: '{}'", (char) ch);
    return ch;
}

void RegExprParser::consume(int expected)
{
    int actual = currentChar();
    consume();
    if (actual != expected)
    {
        throw UnexpectedToken { line_, column_, actual, expected };
    }
}

RegExpr RegExprParser::parse(string_view expr, unsigned line, unsigned column)
{
    input_ = expr;
    currentChar_ = input_.begin();
    line_ = line;
    column_ = column;

    return parseExpr();
}

RegExpr RegExprParser::parseExpr()
{
    return parseLookAheadExpr();
}

RegExpr RegExprParser::parseLookAheadExpr()
{
    RegExpr lhs = parseAlternation();

    if (currentChar() == '/')
    {
        consume();
        RegExpr rhs = parseAlternation();
        lhs = LookAheadExpr { make_unique<RegExpr>(std::move(lhs)), make_unique<RegExpr>(std::move(rhs)) };
    }

    return lhs;
}

RegExpr RegExprParser::parseAlternation()
{
    RegExpr lhs = parseConcatenation();

    while (currentChar() == '|')
    {
        consume();
        RegExpr rhs = parseConcatenation();
        lhs = AlternationExpr { make_unique<RegExpr>(std::move(lhs)), make_unique<RegExpr>(std::move(rhs)) };
    }

    return lhs;
}

RegExpr RegExprParser::parseConcatenation()
{
    // FOLLOW-set, the set of terminal tokens that can occur right after a concatenation
    static const string_view follow = "/|)";
    RegExpr lhs = parseClosure();

    while (!eof() && follow.find(currentChar()) == std::string_view::npos)
    {
        RegExpr rhs = parseClosure();
        lhs =
            ConcatenationExpr { make_unique<RegExpr>(std::move(lhs)), make_unique<RegExpr>(std::move(rhs)) };
    }

    return lhs;
}

RegExpr RegExprParser::parseClosure()
{
    RegExpr subExpr = parseAtom();

    switch (currentChar())
    {
        case '?': consume(); return ClosureExpr { make_unique<RegExpr>(std::move(subExpr)), 0, 1 };
        case '*': consume(); return ClosureExpr { make_unique<RegExpr>(std::move(subExpr)), 0 };
        case '+': consume(); return ClosureExpr { make_unique<RegExpr>(std::move(subExpr)), 1 };
        case '{': {
            consume();
            unsigned int m = parseInt();
            if (currentChar() == ',')
            {
                consume();
                unsigned int n = parseInt();
                consume('}');
                return ClosureExpr { make_unique<RegExpr>(std::move(subExpr)), m, n };
            }
            else
            {
                consume('}');
                return ClosureExpr { make_unique<RegExpr>(std::move(subExpr)), m, m };
            }
        }
        default: return subExpr;
    }
}

unsigned RegExprParser::parseInt()
{
    unsigned n = 0;
    while (isdigit(currentChar()))
    {
        n *= 10;
        n += currentChar() - '0';
        consume();
    }
    return n;
}

RegExpr RegExprParser::parseAtom()
{
    // skip any whitespace (except newlines)
    while (!eof() && isspace(currentChar()) && currentChar() != '\n')
        consume();

    switch (currentChar())
    {
        case std::char_traits<char>::eof(): // EOF
        case ')': return EmptyExpr {};
        case '<':
            consume();
            consume('<');
            consume('E');
            consume('O');
            consume('F');
            consume('>');
            consume('>');
            return EndOfFileExpr {};
        case '(': {
            consume();
            RegExpr subExpr = parseExpr();
            consume(')');
            return subExpr;
        }
        case '"': {
            consume();
            RegExpr lhs = CharacterExpr { consume() };
            while (!eof() && currentChar() != '"')
            {
                RegExpr rhs = CharacterExpr { consume() };
                lhs = ConcatenationExpr { make_unique<RegExpr>(std::move(lhs)),
                                          make_unique<RegExpr>(std::move(rhs)) };
            }
            consume('"');
            return lhs;
        }
        case '[': return parseCharacterClass();
        case '.': consume(); return DotExpr {};
        case '^': consume(); return BeginOfLineExpr {};
        case '$': consume(); return EndOfLineExpr {};
        default: return CharacterExpr { parseSingleCharacter() };
    }
}

RegExpr RegExprParser::parseCharacterClass()
{
    consume();                              // '['
    const bool complement = consumeIf('^'); // TODO

    SymbolSet ss;
    parseCharacterClassFragment(ss);
    while (!eof() && currentChar() != ']')
        parseCharacterClassFragment(ss);

    if (complement)
        ss.complement();

    consume(']');
    return CharacterClassExpr { std::move(ss) };
}

void RegExprParser::parseNamedCharacterClass(SymbolSet& ss)
{
    consume('[');
    consume(':');
    string token;
    while (isalpha(currentChar()))
    {
        token += static_cast<char>(consume());
    }
    consume(':');
    consume(']');

    static const unordered_map<string_view, function<void(SymbolSet&)>> names = {
        { "alnum",
          [](SymbolSet& ss) {
              for (Symbol c = 'a'; c <= 'z'; c++)
                  ss.insert(c);
              for (Symbol c = 'A'; c <= 'Z'; c++)
                  ss.insert(c);
              for (Symbol c = '0'; c <= '9'; c++)
                  ss.insert(c);
          } },
        { "alpha",
          [](SymbolSet& ss) {
              for (Symbol c = 'a'; c <= 'z'; c++)
                  ss.insert(c);
              for (Symbol c = 'A'; c <= 'Z'; c++)
                  ss.insert(c);
          } },
        { "blank",
          [](SymbolSet& ss) {
              ss.insert(' ');
              ss.insert('\t');
          } },
        { "cntrl",
          [](SymbolSet& ss) {
              for (Symbol c = 0; c <= 255; c++)
                  if (iscntrl(c))
                      ss.insert(c);
          } },
        { "digit",
          [](SymbolSet& ss) {
              for (Symbol c = '0'; c <= '9'; c++)
                  ss.insert(c);
          } },
        { "graph",
          [](SymbolSet& ss) {
              for (Symbol c = 0; c <= 255; c++)
                  if (isgraph(c))
                      ss.insert(c);
          } },
        { "lower",
          [](SymbolSet& ss) {
              for (Symbol c = 'a'; c <= 'z'; c++)
                  ss.insert(c);
          } },
        { "print",
          [](SymbolSet& ss) {
              for (Symbol c = 0; c <= 255; c++)
                  if (isprint(c) || c == ' ')
                      ss.insert(c);
          } },
        { "punct",
          [](SymbolSet& ss) {
              for (Symbol c = 0; c <= 255; c++)
                  if (ispunct(c))
                      ss.insert(c);
          } },
        { "space",
          [](SymbolSet& ss) {
              for (Symbol c: "\f\n\r\t\v")
                  ss.insert(c);
          } },
        { "upper",
          [](SymbolSet& ss) {
              for (Symbol c = 'A'; c <= 'Z'; c++)
                  ss.insert(c);
          } },
        { "xdigit",
          [](SymbolSet& ss) {
              for (Symbol c = '0'; c <= '9'; c++)
                  ss.insert(c);
              for (Symbol c = 'a'; c <= 'f'; c++)
                  ss.insert(c);
              for (Symbol c = 'A'; c <= 'F'; c++)
                  ss.insert(c);
          } },
    };

    if (auto i = names.find(token); i != names.end())
        i->second(ss);
    else
        throw UnexpectedToken { line_, column_, token, "<valid character class>" };
}

Symbol RegExprParser::parseSingleCharacter()
{
    if (currentChar() != '\\')
        return consume();

    consume(); // consumes escape character
    switch (currentChar())
    {
        case 'a': consume(); return '\a';
        case 'b': consume(); return '\b';
        case 'f': consume(); return '\f';
        case 'n': consume(); return '\n';
        case 'r': consume(); return '\r';
        case 's': consume(); return ' ';
        case 't': consume(); return '\t';
        case 'v': consume(); return '\v';
        case 'x': {
            consume();

            char buf[3];
            buf[0] = consume();
            if (!isxdigit(buf[0]))
                throw UnexpectedToken { line_, column_, string(1, buf[0]), "[0-9a-fA-F]" };
            buf[1] = consume();
            if (!isxdigit(buf[1]))
                throw UnexpectedToken { line_, column_, string(1, buf[1]), "[0-9a-fA-F]" };
            buf[2] = 0;

            return static_cast<Symbol>(strtoul(buf, nullptr, 16));
        }
        case '0': {
            const Symbol x0 = consume();
            if (!isdigit(currentChar()))
                return '\0';

            // octal value (\DDD)
            char buf[4];
            buf[0] = x0;
            buf[1] = consume();
            if (!(buf[1] >= '0' && buf[1] <= '7'))
                throw UnexpectedToken { line_, column_, string(1, buf[1]), "[0-7]" };
            buf[2] = consume();
            if (!(buf[2] >= '0' && buf[2] <= '7'))
                throw UnexpectedToken { line_, column_, string(1, buf[2]), "[0-7]" };
            buf[3] = '\0';

            return static_cast<Symbol>(strtoul(buf, nullptr, 8));
        }
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            // octal value (\DDD)
            char buf[4];
            buf[0] = consume();
            buf[1] = consume();
            if (!(buf[1] >= '0' && buf[1] <= '7'))
                throw UnexpectedToken { line_, column_, string(1, buf[1]), "[0-7]" };
            buf[2] = consume();
            if (!(buf[2] >= '0' && buf[2] <= '7'))
                throw UnexpectedToken { line_, column_, string(1, buf[2]), "[0-7]" };
            buf[3] = '\0';

            return static_cast<Symbol>(strtoul(buf, nullptr, 8));
        }
        case '"':
        case '$':
        case '(':
        case ')':
        case '*':
        case '+':
        case ':':
        case '?':
        case '[':
        case '\'':
        case '\\':
        case ']':
        case '^':
        case '{':
        case '}':
        case '.':
        case '/': return consume();
        default: {
            throw UnexpectedToken { line_,
                                    column_,
                                    fmt::format("'{}'", static_cast<char>(currentChar())),
                                    "<escape sequence character>" };
        }
    }
}

void RegExprParser::parseCharacterClassFragment(SymbolSet& ss)
{
    // parse [:named:]
    if (currentChar() == '[')
    {
        parseNamedCharacterClass(ss);
        return;
    }

    // parse single char (A) or range (A-Z)
    const Symbol c1 = parseSingleCharacter();
    if (currentChar() != '-')
    {
        ss.insert(c1);
        return;
    }

    consume(); // consume '-'
    const Symbol c2 = parseSingleCharacter();

    for (Symbol c_i = c1; c_i <= c2; c_i++)
        ss.insert(c_i);
}

} // namespace regex_dfa
