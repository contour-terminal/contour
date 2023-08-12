// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/LexerDef.h> // special tags
#include <regex_dfa/RegExpr.h>
#include <regex_dfa/RegExprParser.h>
#include <regex_dfa/RuleParser.h>
#include <regex_dfa/Symbols.h>

#include <cstring>
#include <iostream>
#include <sstream>

using namespace std;

namespace regex_dfa
{

RuleParser::RuleParser(unique_ptr<istream> input, int firstTerminalId):
    _stream { std::move(input) },
    _lastParsedRule { nullptr },
    _lastParsedRuleIsRef { false },
    _currentChar { 0 },
    _line { 1 },
    _column { 0 },
    _offset { 0 },
    _nextTag { firstTerminalId }
{
    consumeChar();
}

RuleParser::RuleParser(string input, int firstTerminalId):
    RuleParser { make_unique<stringstream>(std::move(input)), firstTerminalId }
{
}

RuleList RuleParser::parseRules()
{
    RuleList rules;

    for (;;)
    {
        consumeSpace();
        if (eof())
        {
            break;
        }
        else if (currentChar() == '\n')
        {
            consumeChar();
        }
        else
        {
            parseRule(rules);
        }
    }

    // collect all condition labels, find all <*>-conditions, then replace their <*> with {collected
    // conditions}
    set<string> conditions;
    list<Rule*> starRules;
    for (Rule& rule: rules)
    {
        for (const string& condition: rule.conditions)
        {
            if (condition != "*")
            {
                conditions.emplace(condition);
            }
            else
            {
                rule.conditions.clear();
                starRules.emplace_back(&rule);
            }
        }
    }
    for (Rule* rule: starRules)
        for (const string& condition: conditions)
            rule->conditions.emplace_back(condition);

    return rules;
}

void RuleParser::parseRule(RuleList& rules)
{
    // Rule         ::= RuleConditionList? BasicRule
    //                | RuleConditionList '{' BasicRule* '}' (LF | EOF)?
    // BasicRule    ::= TOKEN RuleOptions? SP '::=' SP RegEx SP? (LF | EOF)
    // RuleOptions  ::= '(' RuleOption (',' RuleOption)*
    // RuleOption   ::= ignore

    consumeSP();
    if (_currentChar == '|' && _lastParsedRule != nullptr)
    {
        consumeChar();
        consumeSP();
        const string pattern = parseExpression();
        _lastParsedRule->pattern += '|' + pattern;
        return;
    }

    // finalize ref-rule by surrounding it with round braces
    if (_lastParsedRuleIsRef)
        _lastParsedRule->pattern = fmt::format("({})", _lastParsedRule->pattern);

    vector<string> conditions = parseRuleConditions();
    consumeSP();
    if (!conditions.empty() && currentChar() == '{')
    {
        consumeChar();
        consumeAnySP(); // allow whitespace, including LFs
        while (!eof() && currentChar() != '}')
        {
            parseBasicRule(rules, vector<string>(conditions));
            consumeSP(); //  part of the next line, allow indentation
        }
        consumeChar('}');
        consumeSP();
        if (currentChar() == '\n')
            consumeChar();
        else if (!eof())
            throw UnexpectedChar { _line, _column, _currentChar, '\n' };
    }
    else
    {
        parseBasicRule(rules, std::move(conditions));
    }
}

struct TestRuleForName
{
    string name;
    bool operator()(const Rule& r) const { return r.name == name; }
};

void RuleParser::parseBasicRule(RuleList& rules, vector<string>&& conditions)
{
    const unsigned int beginLine = _line;
    const unsigned int beginColumn = _column;

    string token = consumeToken();
    bool ignore = false;
    bool ref = false;
    if (_currentChar == '(')
    {
        consumeChar();
        unsigned optionOffset = _offset;
        string option = consumeToken();
        consumeChar(')');

        if (option == "ignore")
            ignore = true;
        else if (option == "ref")
            ref = true;
        else
            throw InvalidRuleOption { optionOffset, option };
    }
    consumeSP();
    consumeAssoc();
    consumeSP();
    const unsigned int line = _line;
    const unsigned int column = _column;
    const string pattern = parseExpression();
    if (currentChar() == '\n')
        consumeChar();
    else if (!eof())
        throw UnexpectedChar { _line, _column, _currentChar, '\n' };

    const Tag tag = [&] {
        if (ignore || ref)
            return IgnoreTag;
        else if (auto i = find_if(rules.begin(), rules.end(), TestRuleForName { token }); i != rules.end())
            return i->tag;
        else
            return _nextTag++;
    }();

    if (ref && !conditions.empty())
        throw InvalidRefRuleWithConditions {
            beginLine, beginColumn, Rule { line, column, tag, std::move(conditions), token, pattern }
        };

    if (conditions.empty())
        conditions.emplace_back("INITIAL");

    sort(conditions.begin(), conditions.end());

    if (!ref)
    {
        if (auto i = find_if(rules.begin(), rules.end(), TestRuleForName { token }); i != rules.end())
        {
            throw DuplicateRule { Rule { line, column, tag, std::move(conditions), token, pattern }, *i };
        }
        else
        {
            rules.emplace_back(Rule { line, column, tag, conditions, token, pattern });
            _lastParsedRule = &rules.back();
            _lastParsedRuleIsRef = false;
        }
    }
    else if (auto i = _refRules.find(token); i != _refRules.end())
    {
        throw DuplicateRule { Rule { line, column, tag, std::move(conditions), token, pattern }, i->second };
    }
    else
    {
        // TODO: throw if !conditions.empty();
        _refRules[token] = { line, column, tag, {}, token, pattern };
        _lastParsedRule = &_refRules[token];
        _lastParsedRuleIsRef = true;
    }
}

vector<string> RuleParser::parseRuleConditions()
{
    // RuleConditionList ::= '<' ('*' | TOKEN (',' SP* TOKEN)) '>'
    if (currentChar() != '<')
        return {};

    consumeChar();

    if (currentChar() == '*')
    {
        consumeChar();
        consumeChar('>');
        return { "*" };
    }

    vector<string> conditions { consumeToken() };

    while (currentChar() == ',')
    {
        consumeChar();
        consumeSP();
        conditions.emplace_back(consumeToken());
    }

    consumeChar('>');

    return conditions;
}

string RuleParser::parseExpression()
{
    // expression ::= " .... "
    //              | ....

    stringstream sstr;

    size_t i = 0;
    size_t lastGraph = 0;
    while (!eof() && _currentChar != '\n')
    {
        if (isgraph(_currentChar))
            lastGraph = i + 1;
        i++;
        sstr << consumeChar();
    }
    string pattern = sstr.str().substr(0, lastGraph); // skips trailing spaces

    // replace all occurrences of {ref}
    for (const pair<const string, Rule>& ref: _refRules)
    {
        const Rule& rule = ref.second;
        const string name = fmt::format("{{{}}}", rule.name);
        // for (size_t i = 0; (i = pattern.find(name, i)) != string::npos; i += rule.pattern.size()) {
        //   pattern.replace(i, name.size(), rule.pattern);
        // }
        size_t i = 0;
        while ((i = pattern.find(name, i)) != string::npos)
        {
            pattern.replace(i, name.size(), rule.pattern);
            i += rule.pattern.size();
        }
    }

    return pattern;
}

// skips space until LF or EOF
void RuleParser::consumeSpace()
{
    for (;;)
    {
        switch (_currentChar)
        {
            case ' ':
            case '\t':
            case '\r': consumeChar(); break;
            case '#':
                while (!eof() && _currentChar != '\n')
                {
                    consumeChar();
                }
                break;
            default: return;
        }
    }
}

char RuleParser::currentChar() const noexcept
{
    return _currentChar;
}

char RuleParser::consumeChar(char ch)
{
    if (_currentChar != ch)
        throw UnexpectedChar { _line, _column, _currentChar, ch };

    return consumeChar();
}

char RuleParser::consumeChar()
{
    char t = _currentChar;

    _currentChar = _stream->get();
    if (!_stream->eof())
    {
        _offset++;
        if (t == '\n')
        {
            _line++;
            _column = 1;
        }
        else
        {
            _column++;
        }
    }

    return t;
}

bool RuleParser::eof() const noexcept
{
    return std::char_traits<char>::eq(_currentChar, std::char_traits<char>::eof()) || _stream->eof();
}

string RuleParser::consumeToken()
{
    stringstream sstr;

    if (!isalpha(_currentChar) || _currentChar == '_')
        throw UnexpectedToken { _offset, _currentChar, "Token" };

    do
        sstr << consumeChar();
    while (isalnum(_currentChar) || _currentChar == '_');

    return sstr.str();
}

void RuleParser::consumeAnySP()
{
    while (_currentChar == ' ' || _currentChar == '\t' || _currentChar == '\n')
        consumeChar();
}

void RuleParser::consumeSP()
{
    while (_currentChar == ' ' || _currentChar == '\t')
        consumeChar();
}

void RuleParser::consumeAssoc()
{
    consumeChar(':');
    consumeChar(':');
    consumeChar('=');
}

} // namespace regex_dfa
