// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Compiler.h>
#include <regex_dfa/DFA.h>
#include <regex_dfa/DFABuilder.h>
#include <regex_dfa/DFAMinimizer.h>
#include <regex_dfa/LexerDef.h>
#include <regex_dfa/MultiDFA.h>
#include <regex_dfa/NFA.h>
#include <regex_dfa/NFABuilder.h>
#include <regex_dfa/RegExpr.h>
#include <regex_dfa/RegExprParser.h>
#include <regex_dfa/Rule.h>
#include <regex_dfa/RuleParser.h>

#include <iostream>

using namespace std;

namespace regex_dfa
{

void Compiler::parse(string text)
{
    parse(make_unique<stringstream>(std::move(text)));
}

void Compiler::parse(unique_ptr<istream> stream)
{
    declareAll(RuleParser { std::move(stream) }.parseRules());
}

void Compiler::declareAll(RuleList rules)
{
    rules_.reserve(rules_.size() + rules.size());

    // populate RegExpr
    for (Rule& rule: rules)
        rule.regexpr = make_unique<RegExpr>(RegExprParser {}.parse(rule.pattern, rule.line, rule.column));

    containsBeginOfLine_ = any_of(rules.begin(), rules.end(), ruleContainsBeginOfLine);

    if (containsBeginOfLine_)
    {
        // We have at least one BOL-rule.
        for (Rule& rule: rules)
        {
            if (!regex_dfa::containsBeginOfLine(*rule.regexpr))
            {
                NFA nfa = NFABuilder {}.construct(*rule.regexpr, rule.tag);
                for (const string& condition: rule.conditions)
                {
                    NFA& fa = fa_[condition];
                    if (fa.empty())
                        fa = nfa.clone();
                    else
                        fa.alternate(nfa.clone());
                }
                declare(rule);
            }
            declare(rule, "_0"); // BOL
        }
    }
    else
    {
        // No BOL-rules present, just declare them then.
        for (Rule& rule: rules)
            declare(rule);
    }

    for (Rule& rule: rules)
    {
        if (auto i = names_.find(rule.tag); i != names_.end() && i->first != rule.tag)
            // Can actually only happen on "ignore" attributed rule count > 1.
            names_[rule.tag] = fmt::format("{}, {}", i->second, rule.name);
        else
            names_[rule.tag] = rule.name;

        rules_.emplace_back(std::move(rule));
    }
}

size_t Compiler::size() const
{
    size_t result = 0;
    for (const pair<const string, NFA>& fa: fa_)
        result += fa.second.size();
    return result;
}

void Compiler::declare(const Rule& rule, const string& conditionSuffix)
{
    NFA nfa = NFABuilder {}.construct(*rule.regexpr, rule.tag);

    for (const string& condition: rule.conditions)
    {
        NFA& fa = fa_[condition + conditionSuffix];

        if (fa.empty())
            fa = nfa.clone();
        else
            fa.alternate(nfa.clone());
    }
}

// const map<string, NFA>& Compiler::automata() const {
//   return fa_;
// }

MultiDFA Compiler::compileMultiDFA(OvershadowMap* overshadows)
{
    map<string, DFA> dfaMap;
    for (const auto& fa: fa_)
        dfaMap[fa.first] = DFABuilder { fa.second.clone() }.construct(overshadows);

    return constructMultiDFA(std::move(dfaMap));
}

DFA Compiler::compileDFA(OvershadowMap* overshadows)
{
    assert((!containsBeginOfLine_ && fa_.size() == 1) || (containsBeginOfLine_ && fa_.size() == 2));
    return DFABuilder { fa_.begin()->second.clone() }.construct(overshadows);
}

DFA Compiler::compileMinimalDFA()
{
    return DFAMinimizer { compileDFA() }.constructDFA();
}

LexerDef Compiler::compile()
{
    return generateTables(compileMinimalDFA(), containsBeginOfLine_, std::move(names_));
}

LexerDef Compiler::compileMulti(OvershadowMap* overshadows)
{
    MultiDFA multiDFA = compileMultiDFA(overshadows);
    multiDFA = DFAMinimizer { multiDFA }.constructMultiDFA();
    return generateTables(multiDFA, containsBeginOfLine_, names());
}

LexerDef Compiler::generateTables(const DFA& dfa, bool requiresBeginOfLine, map<Tag, string> names)
{
    const Alphabet alphabet = dfa.alphabet();
    TransitionMap transitionMap;

    for (StateId state = 0, sE = dfa.lastState(); state <= sE; ++state)
        for (Symbol c: alphabet)
            if (optional<StateId> nextState = dfa.delta(state, c); nextState.has_value())
                transitionMap.define(state, c, nextState.value());

    map<StateId, Tag> acceptStates;
    for (StateId s: dfa.acceptStates())
        acceptStates.emplace(s, *dfa.acceptTag(s));

    // TODO: many initial states !
    return LexerDef { { { "INITIAL", dfa.initialState() } },
                      requiresBeginOfLine,
                      std::move(transitionMap),
                      std::move(acceptStates),
                      dfa.backtracking(),
                      std::move(names) };
}

LexerDef Compiler::generateTables(const MultiDFA& multiDFA, bool requiresBeginOfLine, map<Tag, string> names)
{
    const Alphabet alphabet = multiDFA.dfa.alphabet();
    TransitionMap transitionMap;

    for (StateId state = 0, sE = multiDFA.dfa.lastState(); state <= sE; ++state)
        for (const Symbol c: alphabet)
            if (optional<StateId> nextState = multiDFA.dfa.delta(state, c); nextState.has_value())
                transitionMap.define(state, c, nextState.value());

    map<StateId, Tag> acceptStates;
    for (StateId s: multiDFA.dfa.acceptStates())
        acceptStates.emplace(s, *multiDFA.dfa.acceptTag(s));

    // TODO: many initial states !
    return LexerDef { multiDFA.initialStates,  requiresBeginOfLine,         std::move(transitionMap),
                      std::move(acceptStates), multiDFA.dfa.backtracking(), std::move(names) };
}

} // namespace regex_dfa
