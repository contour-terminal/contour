// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Compiler.h>
#include <regex_dfa/DFA.h>
#include <regex_dfa/DFABuilder.h>
#include <regex_dfa/MultiDFA.h>

#include <catch2/catch.hpp>

#include <memory>
#include <sstream>

using namespace regex_dfa;

TEST_CASE("regex_DFABuilder.shadowing")
{
    Compiler cc;
    cc.parse(std::make_unique<std::stringstream>(R"(
    Identifier  ::= [a-z][a-z0-9]*
    TrueLiteral ::= "true"
  )"));
    // rule 2 is overshadowed by rule 1
    Compiler::OvershadowMap overshadows;
    DFA dfa = cc.compileDFA(&overshadows);
    REQUIRE(1 == overshadows.size());
    CHECK(2 == overshadows[0].first);  // overshadowee
    CHECK(1 == overshadows[0].second); // overshadower
}
