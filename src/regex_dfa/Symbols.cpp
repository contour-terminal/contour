// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Symbols.h>

#include <sstream>

using namespace std;

namespace regex_dfa
{

string prettySymbol(Symbol input)
{
    switch (input)
    {
        case Symbols::Error: return "<<ERROR>>";
        case Symbols::BeginOfLine: return "<<BOL>>";
        case Symbols::EndOfLine: return "<<EOL>>";
        case Symbols::EndOfFile: return "<<EOF>>";
        case Symbols::Epsilon: return "ε";
        case '\a': return "\\a";
        case '\b': return "\\b";
        case '\f': return "\\f";
        case '\n': return "\\n";
        case '\r': return "\\r";
        case ' ': return "\\s";
        case '\t': return "\\t";
        case '\v': return "\\v";
        case '\0': return "\\0";
        case '.': return "\\."; // so we can distinguish from dot-operator
        default:
            if (isprint(input))
            {
                return fmt::format("{}", (char) input);
            }
            else
            {
                return fmt::format("\\x{:02x}", input);
            }
    }
}

string prettyCharRange(Symbol ymin, Symbol ymax)
{
    assert(ymin <= ymax);

    stringstream sstr;
    switch (ymax - ymin)
    {
        case 0: sstr << prettySymbol(ymin); break;
        case 1: sstr << prettySymbol(ymin) << prettySymbol(ymin + 1); break;
        case 2: sstr << prettySymbol(ymin) << prettySymbol(ymin + 1) << prettySymbol(ymax); break;
        default: sstr << prettySymbol(ymin) << '-' << prettySymbol(ymax); break;
    }
    return sstr.str();
}

string groupCharacterClassRanges(const vector<bool>& syms)
{
    // {1,3,5,a,b,c,d,e,f,z]
    // ->
    // {{1}, {3}, {5}, {a-f}, {z}}

    stringstream sstr;
    Symbol ymin = '\0';
    Symbol ymax = ymin;
    int k = 0;

    for (size_t i = 0, e = syms.size(); i != e; ++i)
    {
        if (!syms[i])
            continue;

        const Symbol c = (Symbol) i;
        if (c == ymax + 1)
        { // range growing
            ymax = c;
        }
        else
        { // gap found
            if (k)
            {
                sstr << prettyCharRange(ymin, ymax);
            }
            ymin = ymax = c;
        }
        k++;
    }
    sstr << prettyCharRange(ymin, ymax);

    return sstr.str();
}

string groupCharacterClassRanges(vector<Symbol> chars)
{
    // we took a copy in tgroup here, so I can sort() later
    sort(chars.begin(), chars.end());

    if (chars.size() == 1)
        return prettySymbol(chars.front());

    // {1,3,5,a,b,c,d,e,f,z]
    // ->
    // "123a-fz"

    stringstream sstr;
    Symbol ymin = 0;
    Symbol ymax = ymin;
    int i = 0;

    for (Symbol c: chars)
    {
        if (c == ymax + 1)
        { // range growing
            ymax = c;
        }
        else
        { // gap found
            if (i)
            {
                sstr << prettyCharRange(ymin, ymax);
            }
            ymin = ymax = c;
        }
        i++;
    }
    sstr << prettyCharRange(ymin, ymax);

    return sstr.str();
}

SymbolSet::SymbolSet(DotMode): set_(256, true), size_ { 255 }, hash_ { 2166136261 }
{
    set_[(size_t) '\n'] = false;
    for (Symbol s: *this)
    {
        hash_ = (hash_ * 16777619) ^ s;
    }
}

bool SymbolSet::isDot() const noexcept
{
    static SymbolSet dot(SymbolSet::Dot);
    return *this == dot;
}

string SymbolSet::to_string() const
{
    if (isDot())
        return ".";

    return groupCharacterClassRanges(set_);
}

void SymbolSet::complement()
{
    // flip bits
    for (size_t i = 0, e = set_.size(); i != e; ++i)
    {
        set_[i] = !set_[i];
    }

    // flip size
    size_ = set_.size() - size_;

    recalculateHash();
}

void SymbolSet::recalculateHash()
{
    // recalculate hash
    hash_ = 2166136261;
    for (Symbol s: *this)
    {
        hash_ = (hash_ * 16777619) ^ s;
    }
}

} // namespace regex_dfa
