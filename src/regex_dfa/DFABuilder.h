// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/NFA.h>

#include <map>
#include <utility>
#include <vector>

namespace regex_dfa
{

class DFA;
class State;

class DFABuilder
{
  public:
    //! Map of rules that shows which rule is overshadowed by which other rule.
    using OvershadowMap = std::vector<std::pair<Tag, Tag>>;

    explicit DFABuilder(NFA&& nfa): nfa_ { std::move(nfa) } {}

    /**
     * Constructs a DFA out of the NFA.
     *
     * @param overshadows if not nullptr, it will be used to store semantic information about
     *                    which rule tags have been overshadowed by which.
     */
    [[nodiscard]] DFA construct(OvershadowMap* overshadows = nullptr);

  private:
    struct TransitionTable;

    [[nodiscard]] DFA constructDFA(const std::vector<StateIdVec>& Q,
                                   const TransitionTable& T,
                                   OvershadowMap* overshadows) const;

    /**
     * Finds @p t in @p Q and returns its offset (aka configuration number) or -1 if not found.
     */
    [[nodiscard]] static std::optional<StateId> configurationNumber(const std::vector<StateIdVec>& Q,
                                                                    const StateIdVec& t);

    /**
     * Determines the tag to use for the deterministic state representing @p q from non-deterministic FA @p
     * fa.
     *
     * @param q the set of states that reflect a single state in the DFA equal to the input FA
     *
     * @returns the determined tag or std::nullopt if none
     */
    [[nodiscard]] std::optional<Tag> determineTag(const StateIdVec& q, std::map<Tag, Tag>* overshadows) const;

  private:
    const NFA nfa_;
};

} // namespace regex_dfa
