// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/State.h>

#include <fmt/format.h>

#include <klex/util/testing.h>

TEST(regex_State, to_string)
{
    regex_dfa::StateIdVec v { 1, 2, 3 };
    EXPECT_EQ("{n1, n2, n3}", fmt::format("{}", v));
}
