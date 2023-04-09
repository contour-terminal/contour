// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/Symbols.h>

#include <fmt/format.h>

#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace regex_dfa
{

struct AlternationExpr;
struct BeginOfLineExpr;
struct CharacterClassExpr;
struct CharacterExpr;
struct ClosureExpr;
struct ConcatenationExpr;
struct DotExpr;
struct EmptyExpr;
struct EndOfFileExpr;
struct EndOfLineExpr;
struct LookAheadExpr;

using RegExpr = std::variant<AlternationExpr,
                             BeginOfLineExpr,
                             CharacterClassExpr,
                             CharacterExpr,
                             ClosureExpr,
                             ConcatenationExpr,
                             DotExpr,
                             EmptyExpr,
                             EndOfFileExpr,
                             EndOfLineExpr,
                             LookAheadExpr>;

struct LookAheadExpr
{
    std::unique_ptr<RegExpr> left;
    std::unique_ptr<RegExpr> right;
};

struct AlternationExpr
{
    std::unique_ptr<RegExpr> left;
    std::unique_ptr<RegExpr> right;
};

struct ConcatenationExpr
{
    std::unique_ptr<RegExpr> left;
    std::unique_ptr<RegExpr> right;
};

struct ClosureExpr
{
    std::unique_ptr<RegExpr> subExpr;
    unsigned minimumOccurrences { 0 };
    unsigned maximumOccurrences { std::numeric_limits<unsigned>::max() };
};

struct CharacterExpr
{
    Symbol value;
};

struct CharacterClassExpr
{
    SymbolSet symbols;
};

struct DotExpr
{
};
struct BeginOfLineExpr
{
};
struct EndOfLineExpr
{
};
struct EndOfFileExpr
{
};
struct EmptyExpr
{
};

std::string to_string(const RegExpr& regex);
int precedence(const RegExpr& regex);
bool containsBeginOfLine(const RegExpr& regex);

} // namespace regex_dfa
