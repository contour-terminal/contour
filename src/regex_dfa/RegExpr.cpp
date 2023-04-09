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
            [&](const ClosureExpr& e) {
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
            [](const EndOfFileExpr& e) { return string { "<<EOF>>" }; },
            [](const BeginOfLineExpr& e) { return string { "^" }; },
            [](const EndOfLineExpr& e) { return string { "$" }; },
            [](const CharacterClassExpr& e) { return e.symbols.to_string(); },
            [](const DotExpr& e) { return string { "." }; },
            [](const EmptyExpr& e) { return string {}; },
        },
        re);
}

int precedence(const RegExpr& regex)
{
    return visit(overloaded {
                     [](const AlternationExpr& e) { return 1; },
                     [](const BeginOfLineExpr& e) { return 4; },
                     [](const CharacterClassExpr& e) { return 4; },
                     [](const CharacterExpr& e) { return 4; },
                     [](const ClosureExpr& e) { return 3; },
                     [](const ConcatenationExpr& e) { return 2; },
                     [](const DotExpr& e) { return 4; },
                     [](const EmptyExpr& e) { return 4; },
                     [](const EndOfFileExpr& e) { return 4; },
                     [](const EndOfLineExpr& e) { return 4; },
                     [](const LookAheadExpr& e) { return 0; },
                 },
                 regex);
}

bool containsBeginOfLine(const RegExpr& regex)
{
    return visit(overloaded {
                     [](const AlternationExpr& e) {
                         return containsBeginOfLine(*e.left) || containsBeginOfLine(*e.right);
                     },
                     [](const BeginOfLineExpr& e) { return true; },
                     [](const CharacterClassExpr& e) { return false; },
                     [](const CharacterExpr& e) { return false; },
                     [](const ClosureExpr& e) { return containsBeginOfLine(*e.subExpr); },
                     [](const ConcatenationExpr& e) {
                         return containsBeginOfLine(*e.left) || containsBeginOfLine(*e.right);
                     },
                     [](const DotExpr& e) { return false; },
                     [](const EmptyExpr& e) { return false; },
                     [](const EndOfFileExpr& e) { return false; },
                     [](const EndOfLineExpr& e) { return false; },
                     [](const LookAheadExpr& e) {
                         return containsBeginOfLine(*e.left) || containsBeginOfLine(*e.right);
                     },
                 },
                 regex);
}

} // namespace regex_dfa
