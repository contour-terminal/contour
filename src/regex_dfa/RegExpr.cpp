// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/RegExpr.h>

#include <crispy/overloaded.h>

#include <fmt/format.h>

#include <iostream>
#include <limits>
#include <sstream>

using namespace std;

/*
  REGULAR EXPRESSION SYNTAX:
  --------------------------

  expr                    := alternation
  alternation             := concatenation ('|' concatenation)*
  concatenation           := closure (closure)*
  closure                 := atom ['*' | '?' | '{' NUM [',' NUM] '}']
  atom                    := character | characterClass | '(' expr ')'
  characterClass          := '[' ['^'] characterClassFragment+ ']'
  characterClassFragment  := character | character '-' character
*/

namespace regex_dfa
{

auto embrace(const RegExpr& outer, const RegExpr& inner)
{
    if (precedence(outer) > precedence(inner))
        return "(" + to_string(inner) + ")";
    else
        return to_string(inner);
}

std::string to_string(const RegExpr& re)
{
    return visit(
        overloaded {
            [&](ClosureExpr const& e) {
                stringstream sstr;
                sstr << embrace(re, *e.subExpr);
                if (e.minimumOccurrences == 0 && e.maximumOccurrences == 1)
                    sstr << '?';
                else if (e.minimumOccurrences == 0 && e.maximumOccurrences == numeric_limits<unsigned>::max())
                    sstr << '*';
                else if (e.minimumOccurrences == 1 && e.maximumOccurrences == numeric_limits<unsigned>::max())
                    sstr << '+';
                else
                    sstr << '{' << e.minimumOccurrences << ',' << e.maximumOccurrences << '}';
                return sstr.str();
            },
            [&](const AlternationExpr& e) { return embrace(re, *e.left) + "|" + embrace(re, *e.right); },
            [&](const ConcatenationExpr& e) { return embrace(re, *e.left) + embrace(re, *e.right); },
            [&](const LookAheadExpr& e) { return embrace(re, *e.left) + "/" + embrace(re, *e.right); },
            [](const CharacterExpr& e) { return string(1, e.value); },
            [](EndOfFileExpr) { return string { "<<EOF>>" }; },
            [](BeginOfLineExpr) { return string { "^" }; },
            [](EndOfLineExpr) { return string { "$" }; },
            [](CharacterClassExpr const& e) { return e.symbols.to_string(); },
            [](DotExpr) { return string { "." }; },
            [](EmptyExpr) { return string {}; },
        },
        re);
}

int precedence(const RegExpr& regex)
{
    return visit(overloaded {
                     [](const AlternationExpr&) { return 1; },
                     [](const BeginOfLineExpr&) { return 4; },
                     [](const CharacterClassExpr&) { return 4; },
                     [](const CharacterExpr&) { return 4; },
                     [](const ClosureExpr&) { return 3; },
                     [](const ConcatenationExpr&) { return 2; },
                     [](const DotExpr&) { return 4; },
                     [](const EmptyExpr&) { return 4; },
                     [](const EndOfFileExpr&) { return 4; },
                     [](const EndOfLineExpr&) { return 4; },
                     [](const LookAheadExpr&) { return 0; },
                 },
                 regex);
}

bool containsBeginOfLine(const RegExpr& regex)
{
    return visit(overloaded {
                     [](const AlternationExpr& e) {
                         return containsBeginOfLine(*e.left) || containsBeginOfLine(*e.right);
                     },
                     [](const BeginOfLineExpr&) { return true; },
                     [](const CharacterClassExpr&) { return false; },
                     [](const CharacterExpr&) { return false; },
                     [](const ClosureExpr& e) { return containsBeginOfLine(*e.subExpr); },
                     [](const ConcatenationExpr& e) {
                         return containsBeginOfLine(*e.left) || containsBeginOfLine(*e.right);
                     },
                     [](const DotExpr&) { return false; },
                     [](const EmptyExpr&) { return false; },
                     [](const EndOfFileExpr&) { return false; },
                     [](const EndOfLineExpr&) { return false; },
                     [](const LookAheadExpr& e) {
                         return containsBeginOfLine(*e.left) || containsBeginOfLine(*e.right);
                     },
                 },
                 regex);
}

} // namespace regex_dfa
