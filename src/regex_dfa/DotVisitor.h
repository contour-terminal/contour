// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <regex_dfa/State.h>

#include <string_view>

namespace regex_dfa
{

class DotVisitor
{
  public:
    virtual ~DotVisitor() = default;

    virtual void start(StateId initialState) = 0;
    virtual void visitNode(StateId number, bool start, bool accept) = 0;
    virtual void visitEdge(StateId from, StateId to, Symbol s) = 0;
    virtual void endVisitEdge(StateId from, StateId to) = 0;
    virtual void end() = 0;
};

} // namespace regex_dfa
