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
    char currentChar() const noexcept;
    char consumeChar(char ch);
    char consumeChar();
    bool eof() const noexcept;
    std::string replaceRefs(const std::string& pattern);

  private:
    std::unique_ptr<std::istream> stream_;
    std::map<std::string, Rule> refRules_;
    Rule* lastParsedRule_;
    bool lastParsedRuleIsRef_;
    char currentChar_;
    unsigned int line_;
    unsigned int column_;
    unsigned int offset_;
    int nextTag_;
};

class RuleParser::InvalidRefRuleWithConditions: public std::runtime_error
{
  public:
    InvalidRefRuleWithConditions(unsigned line, unsigned column, Rule&& rule):
        std::runtime_error { fmt::format(
            "{}:{}: Invalid rule \"{}\". Reference rules must not be labelled with conditions.",
            line,
            column,
            rule.name) },
        rule_ { std::move(rule) }
    {
    }

    const Rule& rule() const noexcept { return rule_; }

  private:
    const Rule rule_;
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
        duplicate_ { std::move(duplicate) },
        other_ { other }
    {
    }

    const Rule& duplicate() const noexcept { return duplicate_; }
    const Rule& other() const noexcept { return other_; }

  private:
    const Rule duplicate_;
    const Rule& other_;
};

class RuleParser::UnexpectedToken: public std::runtime_error
{
  public:
    UnexpectedToken(unsigned offset, char actual, std::string expected):
        std::runtime_error { fmt::format(
            "{}: Unexpected token {}, expected <{}> instead.", offset, actual, expected) },
        offset_ { offset },
        actual_ { std::move(actual) },
        expected_ { std::move(expected) }
    {
    }

    unsigned offset() const noexcept { return offset_; }
    char actual() const noexcept { return actual_; }
    const std::string& expected() const noexcept { return expected_; }

  private:
    unsigned offset_;
    char actual_;
    std::string expected_;
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
        line_ { line },
        column_ { column },
        actual_ { actual },
        expected_ { expected }
    {
    }

    unsigned int line() const noexcept { return line_; }
    unsigned int column() const noexcept { return column_; }
    char actual() const noexcept { return actual_; }
    char expected() const noexcept { return expected_; }

  private:
    static std::string quoted(char ch)
    {
        if (ch < 0)
            return "<<EOF>>";
        else
            return fmt::format("'{}'", ch);
    }

  private:
    unsigned int line_;
    unsigned int column_;
    char actual_;
    char expected_;
};

class RuleParser::InvalidRuleOption: public std::runtime_error
{
  public:
    InvalidRuleOption(unsigned offset, std::string option):
        std::runtime_error { fmt::format("{}: Invalid rule option \"{}\".", offset, option) },
        offset_ { offset },
        option_ { option }
    {
    }

    unsigned offset() const noexcept { return offset_; }
    const std::string& option() const noexcept { return option_; }

  private:
    unsigned offset_;
    std::string option_;
};

} // namespace regex_dfa
