// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/RegExpr.h>
#include <regex_dfa/Symbols.h>

#include <fmt/format.h>

#include <memory>
#include <string_view>

namespace regex_dfa
{

class SymbolSet;

class RegExprParser
{
  public:
    RegExprParser();

    RegExpr parse(std::string_view expr, int line, int column);

    RegExpr parse(std::string_view expr) { return parse(std::move(expr), 1, 1); }

    class UnexpectedToken: public std::runtime_error
    {
      public:
        UnexpectedToken(unsigned int line, unsigned int column, std::string actual, std::string expected):
            std::runtime_error { fmt::format(
                "[{}:{}] Unexpected token {}. Expected {} instead.", line, column, actual, expected) },
            line_ { line },
            column_ { column },
            actual_ { std::move(actual) },
            expected_ { std::move(expected) }
        {
        }

        UnexpectedToken(unsigned int line, unsigned int column, int actual, int expected):
            UnexpectedToken { line,
                              column,
                              actual == -1 ? "EOF" : fmt::format("{}", static_cast<char>(actual)),
                              std::string(1, static_cast<char>(expected)) }
        {
        }

        unsigned int line() const noexcept { return line_; }
        unsigned int column() const noexcept { return column_; }
        const std::string& actual() const noexcept { return actual_; }
        const std::string& expected() const noexcept { return expected_; }

      private:
        unsigned int line_;
        unsigned int column_;
        std::string actual_;
        std::string expected_;
    };

  private:
    int currentChar() const;
    bool eof() const noexcept { return currentChar() == -1; }
    bool consumeIf(int ch);
    void consume(int ch);
    int consume();
    unsigned parseInt();

    RegExpr parse();                                 // expr
    RegExpr parseExpr();                             // lookahead
    RegExpr parseLookAheadExpr();                    // alternation ('/' alternation)?
    RegExpr parseAlternation();                      // concatenation ('|' concatenation)*
    RegExpr parseConcatenation();                    // closure (closure)*
    RegExpr parseClosure();                          // atom ['*' | '?' | '{' NUM [',' NUM] '}']
    RegExpr parseAtom();                             // character | characterClass | '(' expr ')'
    RegExpr parseCharacterClass();                   // '[' characterClassFragment+ ']'
    void parseCharacterClassFragment(SymbolSet& ss); // namedClass | character | character '-' character
    void parseNamedCharacterClass(SymbolSet& ss);    // '[' ':' NAME ':' ']'
    Symbol parseSingleCharacter();

  private:
    std::string_view input_;
    std::string_view::iterator currentChar_;
    unsigned int line_;
    unsigned int column_;
};

} // namespace regex_dfa
