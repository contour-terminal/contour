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

    [[nodiscard]] RegExpr parse(std::string_view expr, unsigned line, unsigned column);

    [[nodiscard]] RegExpr parse(std::string_view expr) { return parse(expr, 1, 1); }

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
                              std::char_traits<char>::eq(actual, std::char_traits<char>::eof())
                                  ? "EOF"
                                  : fmt::format("{}", static_cast<char>(actual)),
                              std::string(1, static_cast<char>(expected)) }
        {
        }

        [[nodiscard]] unsigned int line() const noexcept { return line_; }
        [[nodiscard]] unsigned int column() const noexcept { return column_; }
        [[nodiscard]] const std::string& actual() const noexcept { return actual_; }
        [[nodiscard]] const std::string& expected() const noexcept { return expected_; }

      private:
        unsigned int line_;
        unsigned int column_;
        std::string actual_;
        std::string expected_;
    };

  private:
    [[nodiscard]] int currentChar() const;
    [[nodiscard]] bool eof() const noexcept
    {
        return std::char_traits<char>::eq(currentChar(), std::char_traits<char>::eof());
    }
    [[nodiscard]] bool consumeIf(int ch);
    void consume(int ch);
    int consume();
    [[nodiscard]] unsigned parseInt();

    [[nodiscard]] RegExpr parse();                   // expr
    [[nodiscard]] RegExpr parseExpr();               // lookahead
    [[nodiscard]] RegExpr parseLookAheadExpr();      // alternation ('/' alternation)?
    [[nodiscard]] RegExpr parseAlternation();        // concatenation ('|' concatenation)*
    [[nodiscard]] RegExpr parseConcatenation();      // closure (closure)*
    [[nodiscard]] RegExpr parseClosure();            // atom ['*' | '?' | '{' NUM [',' NUM] '}']
    [[nodiscard]] RegExpr parseAtom();               // character | characterClass | '(' expr ')'
    [[nodiscard]] RegExpr parseCharacterClass();     // '[' characterClassFragment+ ']'
    void parseCharacterClassFragment(SymbolSet& ss); // namedClass | character | character '-' character
    void parseNamedCharacterClass(SymbolSet& ss);    // '[' ':' NAME ':' ']'
    [[nodiscard]] Symbol parseSingleCharacter();

  private:
    std::string_view input_;
    std::string_view::iterator currentChar_;
    unsigned int line_;
    unsigned int column_;
};

} // namespace regex_dfa
