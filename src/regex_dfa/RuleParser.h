// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/Rule.h>

#include <fmt/format.h>

#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace regex_dfa
{

class RuleParser
{
  public:
    explicit RuleParser(std::unique_ptr<std::istream> input, int firstTerminalId = FirstUserTag);
    explicit RuleParser(std::string input, int firstTerminalId = FirstUserTag);

    RuleList parseRules();

    class UnexpectedChar;
    class UnexpectedToken;
    class InvalidRuleOption;
    class InvalidRefRuleWithConditions;
    class DuplicateRule;

  private:
    void parseRule(RuleList& rules);
    std::vector<std::string> parseRuleConditions();
    void parseBasicRule(RuleList& rules, std::vector<std::string>&& conditions);
    std::string parseExpression();

  private:
    std::string consumeToken();
    void consumeAnySP();
    void consumeSP();
    void consumeAssoc();
    void consumeSpace();
    [[nodiscard]] char currentChar() const noexcept;
    char consumeChar(char ch);
    char consumeChar();
    [[nodiscard]] bool eof() const noexcept;
    [[nodiscard]] std::string replaceRefs(const std::string& pattern);

  private:
    std::unique_ptr<std::istream> _stream;
    std::map<std::string, Rule> _refRules;
    Rule* _lastParsedRule;
    bool _lastParsedRuleIsRef;
    char _currentChar;
    unsigned int _line;
    unsigned int _column;
    unsigned int _offset;
    int _nextTag;
};

class RuleParser::InvalidRefRuleWithConditions: public std::runtime_error
{
  public:
    InvalidRefRuleWithConditions(unsigned line, unsigned column, Rule rule):
        std::runtime_error { fmt::format(
            "{}:{}: Invalid rule \"{}\". Reference rules must not be labelled with conditions.",
            line,
            column,
            rule.name) },
        _rule { std::move(rule) }
    {
    }

    [[nodiscard]] Rule const& rule() const noexcept { return _rule; }

  private:
    Rule _rule;
};

class RuleParser::DuplicateRule: public std::runtime_error
{
  public:
    DuplicateRule(Rule&& duplicate, const Rule& other):
        std::runtime_error { fmt::format(
            "{}:{}: Duplicated rule definition with name \"{}\", previously defined in {}:{}.",
            duplicate.line,
            duplicate.column,
            duplicate.name,
            other.line,
            other.column) },
        _duplicate { std::move(duplicate) },
        _other { other }
    {
    }

    [[nodiscard]] Rule const& duplicate() const noexcept { return _duplicate; }
    [[nodiscard]] Rule const& other() const noexcept { return _other; }

  private:
    Rule _duplicate;
    Rule const& _other;
};

class RuleParser::UnexpectedToken: public std::runtime_error
{
  public:
    UnexpectedToken(unsigned offset, char actual, std::string expected):
        std::runtime_error { fmt::format(
            "{}: Unexpected token {}, expected <{}> instead.", offset, actual, expected) },
        _offset { offset },
        _actual { actual },
        _expected { std::move(expected) }
    {
    }

    [[nodiscard]] unsigned offset() const noexcept { return _offset; }
    [[nodiscard]] char actual() const noexcept { return _actual; }
    [[nodiscard]] const std::string& expected() const noexcept { return _expected; }

  private:
    unsigned _offset;
    char _actual;
    std::string _expected;
};

class RuleParser::UnexpectedChar: public std::runtime_error
{
  public:
    UnexpectedChar(unsigned int line, unsigned int column, char actual, char expected):
        std::runtime_error { fmt::format("[{}:{}] Unexpected char {}, expected {} instead.",
                                         line,
                                         column,
                                         quoted(actual),
                                         quoted(expected)) },
        _line { line },
        _column { column },
        _actual { actual },
        _expected { expected }
    {
    }

    [[nodiscard]] unsigned int line() const noexcept { return _line; }
    [[nodiscard]] unsigned int column() const noexcept { return _column; }
    [[nodiscard]] char actual() const noexcept { return _actual; }
    [[nodiscard]] char expected() const noexcept { return _expected; }

  private:
    static std::string quoted(char ch)
    {
        if (std::char_traits<char>::eq(ch, std::char_traits<char>::eof()))
            return "<<EOF>>";
        else
            return fmt::format("'{}'", static_cast<char>(ch));
    }

  private:
    unsigned int _line;
    unsigned int _column;
    char _actual;
    char _expected;
};

class RuleParser::InvalidRuleOption: public std::runtime_error
{
  public:
    InvalidRuleOption(unsigned offset, std::string option):
        std::runtime_error { fmt::format("{}: Invalid rule option \"{}\".", offset, option) },
        _offset { offset },
        _option { option }
    {
    }

    [[nodiscard]] unsigned offset() const noexcept { return _offset; }
    [[nodiscard]] const std::string& option() const noexcept { return _option; }

  private:
    unsigned _offset;
    std::string _option;
};

} // namespace regex_dfa
