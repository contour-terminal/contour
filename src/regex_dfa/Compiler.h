// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/DFABuilder.h>
#include <regex_dfa/LexerDef.h>
#include <regex_dfa/NFA.h>
#include <regex_dfa/Rule.h>
#include <regex_dfa/State.h>

#include <istream>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace regex_dfa
{

struct MultiDFA;

/**
 * Top-Level API for compiling lexical patterns into table definitions for Lexer.
 *
 * @see Lexer
 */
class Compiler
{
  public:
    using TagNameMap = std::map<Tag, std::string>;
    using OvershadowMap = DFABuilder::OvershadowMap;
    using AutomataMap = std::map<std::string, NFA>;

    Compiler(): rules_ {}, containsBeginOfLine_ { false }, fa_ {}, names_ {} {}

    /**
     * Parses a @p stream of textual rule definitions to construct their internal data structures.
     */
    void parse(std::unique_ptr<std::istream> stream);
    void parse(std::string text);

    /**
     * Parses a list of @p rules to construct their internal data structures.
     */
    void declareAll(RuleList rules);

    const RuleList& rules() const noexcept { return rules_; }
    const TagNameMap& names() const noexcept { return names_; }
    size_t size() const;

    /**
     * Compiles all previousely parsed rules into a DFA.
     */
    DFA compileDFA(OvershadowMap* overshadows = nullptr);
    MultiDFA compileMultiDFA(OvershadowMap* overshadows = nullptr);

    /**
     * Compiles all previousely parsed rules into a minimal DFA.
     */
    DFA compileMinimalDFA();

    /**
     * Compiles all previousely parsed rules into a suitable data structure for Lexer.
     *
     * @see Lexer
     */
    LexerDef compile();

    /**
     * Compiles all previousely parsed rules into a suitable data structure for Lexer, taking care of
     * multiple conditions as well as begin-of-line.
     */
    LexerDef compileMulti(OvershadowMap* overshadows = nullptr);

    /**
     * Translates the given DFA @p dfa with a given TagNameMap @p names into trivial table mappings.
     *
     * @see Lexer
     */
    static LexerDef generateTables(const DFA& dfa, bool requiresBeginOfLine, const TagNameMap& names);
    static LexerDef generateTables(const MultiDFA& dfa, bool requiresBeginOfLine, const TagNameMap& names);

    const std::map<std::string, NFA>& automata() const { return fa_; }

    bool containsBeginOfLine() const noexcept { return containsBeginOfLine_; }

  private:
    /**
     * Parses a single @p rule to construct their internal data structures.
     */
    void declare(const Rule& rule, const std::string& conditionSuffix = "");

  private:
    RuleList rules_;
    bool containsBeginOfLine_;
    AutomataMap fa_;
    TagNameMap names_;
};

} // namespace regex_dfa
