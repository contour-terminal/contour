// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/DotWriter.h>

#include <sstream>

#include <klex/util/testing.h>

using namespace std;
using namespace regex_dfa;

TEST(regex_DotWriter, simple)
{
    stringstream sstr;
    DotWriter dw(sstr, "n");

    dw.start(0);
    dw.visitNode(0, true, true);
    dw.visitEdge(0, 1, 'a');
    dw.endVisitEdge(0, 1);

    dw.visitNode(1, false, true);
    dw.visitEdge(1, 1, 'b');
    dw.visitEdge(1, 1, '\r');
    dw.visitEdge(1, 1, '\n');
    dw.visitEdge(1, 1, '\t');
    dw.visitEdge(1, 1, ' ');
    dw.endVisitEdge(1, 1);
    dw.end();

    log(sstr.str());
    ASSERT_TRUE(!sstr.str().empty());
    // just make sure it processes
}

TEST(regex_DotWriter, multidfa_simple)
{
    stringstream sstr;
    const MultiDFA::InitialStateMap mis { { "foo", 1 }, { "bar", 2 } };
    DotWriter dw(sstr, "n", mis);

    dw.start(0);
    dw.visitNode(0, true, false);
    dw.visitNode(1, false, true);
    dw.visitNode(2, false, true);

    dw.visitEdge(0, 1, 0x01);
    dw.endVisitEdge(0, 1);

    dw.visitEdge(0, 2, 0x02);
    dw.endVisitEdge(0, 2);

    dw.visitEdge(1, 1, 'a');
    dw.endVisitEdge(1, 1);

    dw.visitEdge(2, 2, 'a');
    dw.endVisitEdge(2, 2);

    dw.end();

    log(sstr.str());
    ASSERT_TRUE(!sstr.str().empty());
    // just make sure it processes
}
