// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace regex_dfa
{

//! input symbol as used for transitions
using Symbol = int;

[[nodiscard]] std::string prettySymbol(Symbol input);
[[nodiscard]] std::string prettyCharRange(Symbol ymin, Symbol ymax);
[[nodiscard]] std::string groupCharacterClassRanges(const std::vector<bool>& syms);
[[nodiscard]] std::string groupCharacterClassRanges(std::vector<Symbol> syms);

// new way of wrapping up Symbols
struct Symbols
{
    // NOLINTBEGIN(readability-identifier-naming)
    constexpr static Symbol Epsilon = -1;
    constexpr static Symbol Error = -2;
    constexpr static Symbol BeginOfLine = -3;
    constexpr static Symbol EndOfLine = -4;
    constexpr static Symbol EndOfFile = -5;
    constexpr static Symbol Character(char ch) { return Symbol(ch); }
    // NOLINTEND(readability-identifier-naming)

    [[nodiscard]] constexpr static bool isSpecial(Symbol s)
    {
        switch (s)
        {
            case Symbols::EndOfFile:
            case Symbols::EndOfLine:
            case Symbols::BeginOfLine:
            case Symbols::Epsilon:
            case Symbols::Error: return true;
            default: return false;
        }
    }
};

/**
 * Represents a set of symbols.
 */
class SymbolSet
{
  public:
    enum DotMode
    {
        Dot
    };

    explicit SymbolSet(DotMode);
    SymbolSet(): set_(256, false), size_ { 0 }, hash_ { 2166136261 } {}

    explicit SymbolSet(std::initializer_list<Symbol> list): SymbolSet()
    {
        std::for_each(list.begin(), list.end(), [this](Symbol s) { insert(s); });
    }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_t size() const noexcept { return size_; }

    //! Transforms into the complement set.
    void complement();

    //! Inserts given Symbol @p s into this set.
    void insert(Symbol s)
    {
        if (!contains(s))
        {
            set_[s] = true;
            hash_ = (hash_ * 16777619) ^ s;
            size_++;
        }
    }

    //! Inserts a range of Simples between [a, b].
    void insert(const std::pair<Symbol, Symbol>& range)
    {
        for (Symbol s = range.first; s <= range.second; ++s)
        {
            insert(s);
        }
    }

    //! @returns whether or not given Symbol @p s is in this set.
    [[nodiscard]] bool contains(Symbol s) const
    {
        assert(s >= 0 && s <= 255 && "Only ASCII allowed.");
        return set_[(size_t) s];
    }

    //! Tests whether or not this SymbolSet can be represented as dot (.), i.e. all but \n.
    [[nodiscard]] bool isDot() const noexcept;

    //! @returns a human readable representation of this set
    [[nodiscard]] std::string to_string() const;

    bool operator==(const SymbolSet& rhs) const noexcept { return hash_ == rhs.hash_ && set_ == rhs.set_; }
    bool operator!=(const SymbolSet& rhs) const noexcept { return !(*this == rhs); }

    class const_iterator // NOLINT(readability-identifier-naming)
    {                    // {{{
      public:
        const_iterator(std::vector<bool>::const_iterator beg,
                       std::vector<bool>::const_iterator end,
                       size_t n):
            beg_ { beg }, end_ { end }, offset_ { n }
        {
            while (beg_ != end_ && !*beg_)
            {
                ++beg_;
                ++offset_;
            }
        }

        Symbol operator*() const { return static_cast<Symbol>(offset_); }

        const_iterator& operator++(int)
        {
            do
            {
                ++beg_;
                ++offset_;
            } while (beg_ != end_ && !*beg_);
            return *this;
        }

        const_iterator& operator++()
        {
            do
            {
                beg_++;
                offset_++;
            } while (beg_ != end_ && !*beg_);
            return *this;
        }

        bool operator==(const const_iterator& rhs) const noexcept { return beg_ == rhs.beg_; }
        bool operator!=(const const_iterator& rhs) const noexcept { return beg_ != rhs.beg_; }

      private:
        std::vector<bool>::const_iterator beg_;
        std::vector<bool>::const_iterator end_;
        size_t offset_;
    }; // }}}

    [[nodiscard]] const_iterator begin() const { return const_iterator(set_.begin(), set_.end(), 0); }
    [[nodiscard]] const_iterator end() const { return const_iterator(set_.end(), set_.end(), set_.size()); }

    [[nodiscard]] size_t hash() const noexcept { return hash_; }

  private:
    void recalculateHash();

  private:
    // XXX we chose vector<bool> as it is an optimized bit vector
    std::vector<bool> set_;
    size_t size_;
    size_t hash_;
};

} // namespace regex_dfa

namespace fmt
{
template <>
struct formatter<regex_dfa::SymbolSet>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const regex_dfa::SymbolSet& v, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", v.to_string());
    }
};
} // namespace fmt

namespace std
{
template <>
struct hash<regex_dfa::SymbolSet>
{
    size_t operator()(const regex_dfa::SymbolSet& set) const { return set.hash(); }
};
} // namespace std
