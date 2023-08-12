// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Alphabet.h>
#include <regex_dfa/NFA.h>
#include <regex_dfa/State.h>

#include <catch2/catch.hpp>

using namespace std;
using namespace regex_dfa;

TEST_CASE("regex_NFA.emptyCtor")
{
    const NFA nfa;
    REQUIRE(0 == nfa.size());
    REQUIRE(nfa.empty());
}

TEST_CASE("regex_NFA.characterCtor")
{
    const NFA nfa { 'a' };
    REQUIRE(2 == nfa.size());
    REQUIRE(0 == nfa.initialStateId());
    REQUIRE(1 == nfa.acceptStateId());
    REQUIRE(StateIdVec { 1 } == nfa.delta(StateIdVec { 0 }, 'a'));
}

TEST_CASE("regex_NFA.concatenate")
{
    const NFA ab = std::move(NFA { 'a' }.concatenate(NFA { 'b' }));
    REQUIRE(4 == ab.size());
    REQUIRE(0 == ab.initialStateId());
    REQUIRE(3 == ab.acceptStateId());

    // TODO: check ab.initial == A.initial
    // TODO: check A.accept == B.initial
    // TODO: check ab.accept == B.accept
}

TEST_CASE("regex_NFA.alternate")
{
    const NFA ab = std::move(NFA { 'a' }.alternate(NFA { 'b' }));
    REQUIRE(6 == ab.size());
    REQUIRE(2 == ab.initialStateId());
    REQUIRE(3 == ab.acceptStateId());

    // TODO: check acceptState transitions to A and B
    // TODO: check A and B's outgoing edges to final acceptState
}

TEST_CASE("regex_NFA.epsilonClosure")
{
    const NFA nfa { 'a' };
    REQUIRE(0 == nfa.initialStateId());
    REQUIRE(1 == nfa.acceptStateId());
    REQUIRE(StateIdVec { 0 } == nfa.epsilonClosure(StateIdVec { 0 }));

    const NFA abc =
        std::move(NFA { 'a' }.concatenate(std::move(NFA { 'b' }.alternate(NFA { 'c' }).recurring())));
    REQUIRE(StateIdVec { 0 } == abc.epsilonClosure(StateIdVec { 0 }));

    const StateIdVec e1 { 1, 2, 4, 6, 8, 9 };
    REQUIRE(e1 == abc.epsilonClosure(StateIdVec { 1 }));
}

TEST_CASE("regex_NFA.delta")
{
    const NFA nfa { 'a' };
    REQUIRE(0 == nfa.initialStateId());
    REQUIRE(1 == nfa.acceptStateId());
    REQUIRE(StateIdVec { 1 } == nfa.delta(StateIdVec { 0 }, 'a'));
}

TEST_CASE("regex_NFA.alphabet")
{
    REQUIRE("{}" == NFA {}.alphabet().to_string());
    REQUIRE("{a}" == NFA { 'a' }.alphabet().to_string());
    REQUIRE("{ab}" == NFA { 'a' }.concatenate(NFA { 'b' }).alphabet().to_string());
    REQUIRE("{abc}" == NFA { 'a' }.concatenate(NFA { 'b' }).alternate(NFA { 'c' }).alphabet().to_string());
}
