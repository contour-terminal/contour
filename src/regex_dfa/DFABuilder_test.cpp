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

#include <memory>
#include <sstream>

#include <klex/util/testing.h>

using namespace regex_dfa;

TEST(regex_DFABuilder, shadowing)
{
    Compiler cc;
    cc.parse(std::make_unique<std::stringstream>(R"(
    Identifier  ::= [a-z][a-z0-9]*
    TrueLiteral ::= "true"
  )"));
    // rule 2 is overshadowed by rule 1
    Compiler::OvershadowMap overshadows;
    DFA dfa = cc.compileDFA(&overshadows);
    ASSERT_EQ(1, overshadows.size());
    EXPECT_EQ(2, overshadows[0].first);  // overshadowee
    EXPECT_EQ(1, overshadows[0].second); // overshadower
}
