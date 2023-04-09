// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/DFA.h>
#include <regex_dfa/NFABuilder.h>

using namespace std;

namespace regex_dfa
{

NFA NFABuilder::construct(const RegExpr& re, Tag tag)
{
    visit(*this, re);

    // fa_.setAccept(acceptState_.value_or(fa_.acceptStateId()), tag);
    if (acceptState_)
        fa_.setAccept(acceptState_.value(), tag);
    else
        fa_.setAccept(tag);

    return move(fa_);
}

NFA NFABuilder::construct(const RegExpr& re)
{
    visit(*this, re);
    return move(fa_);
}

void NFABuilder::operator()(const LookAheadExpr& lookaheadExpr)
{
    // fa_ = move(construct(lookaheadExpr.leftExpr()).lookahead(construct(lookaheadExpr.rightExpr())));
    NFA lhs = construct(*lookaheadExpr.left);
    NFA rhs = construct(*lookaheadExpr.right);
    lhs.lookahead(move(rhs));
    fa_ = move(lhs);
}

void NFABuilder::operator()(const AlternationExpr& alternationExpr)
{
    NFA lhs = construct(*alternationExpr.left);
    NFA rhs = construct(*alternationExpr.right);
    lhs.alternate(move(rhs));
    fa_ = move(lhs);
}

void NFABuilder::operator()(const ConcatenationExpr& concatenationExpr)
{
    NFA lhs = construct(*concatenationExpr.left);
    NFA rhs = construct(*concatenationExpr.right);
    lhs.concatenate(move(rhs));
    fa_ = move(lhs);
}

void NFABuilder::operator()(const CharacterExpr& characterExpr)
{
    fa_ = NFA { characterExpr.value };
}

void NFABuilder::operator()(const CharacterClassExpr& characterClassExpr)
{
    fa_ = NFA { characterClassExpr.symbols };
}

void NFABuilder::operator()(const ClosureExpr& closureExpr)
{
    const unsigned xmin = closureExpr.minimumOccurrences;
    const unsigned xmax = closureExpr.maximumOccurrences;
    constexpr unsigned Infinity = numeric_limits<unsigned>::max();

    if (xmin == 0 && xmax == 1)
        fa_ = move(construct(*closureExpr.subExpr).optional());
    else if (xmin == 0 && xmax == Infinity)
        fa_ = move(construct(*closureExpr.subExpr).recurring());
    else if (xmin == 1 && xmax == Infinity)
        fa_ = move(construct(*closureExpr.subExpr).positive());
    else if (xmin < xmax)
        fa_ = move(construct(*closureExpr.subExpr).repeat(xmin, xmax));
    else if (xmin == xmax)
        fa_ = move(construct(*closureExpr.subExpr).times(xmin));
    else
        throw invalid_argument { "closureExpr" };
}

void NFABuilder::operator()(const BeginOfLineExpr&)
{
    fa_ = NFA { Symbols::Epsilon };
}

void NFABuilder::operator()(const EndOfLineExpr& eolExpr)
{
    // NFA lhs;
    // NFA rhs{'\n'};
    // lhs.lookahead(move(rhs));
    // fa_ = move(lhs);
    fa_ = move(NFA {}.lookahead(NFA { '\n' }));
}

void NFABuilder::operator()(const EndOfFileExpr& eofExpr)
{
    fa_ = NFA { Symbols::EndOfFile };
}

void NFABuilder::operator()(const DotExpr& dotExpr)
{
    // any character except LF
    fa_ = NFA { '\t' };
    for (int ch = 32; ch < 127; ++ch)
    {
        fa_.addTransition(fa_.initialStateId(), ch, fa_.acceptStateId());
    }
}

void NFABuilder::operator()(const EmptyExpr& emptyExpr)
{
    fa_ = NFA { Symbols::Epsilon };
}

} // namespace regex_dfa
