// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/Lexer.h> // TokenInfo: TODO: remove that header/API (inline TokenInfo here then)
#include <regex_dfa/LexerDef.h>

#include <fmt/format.h>

#include <cassert>
#include <climits>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace regex_dfa
{

//! Runtime exception that is getting thrown when a word could not be recognized.
struct LexerError: public std::runtime_error
{
    explicit LexerError(unsigned int _offset):
        std::runtime_error { fmt::format("[{}] Failed to lexically recognize a word.", _offset) },
        offset { _offset }
    {
    }

    unsigned int offset;
};

template <typename Token = Tag,
          typename Machine = StateId,
          const bool RequiresBeginOfLine = true,
          const bool Trace = false>
class LexerIterator
{
  public:
    using TokenInfo = regex_dfa::TokenInfo<Token>;
    using TraceFn = std::function<void(const std::string&)>;

    using difference_type = long;
    using value_type = TokenInfo;
    using pointer = TokenInfo*;
    using reference = TokenInfo&;
    using iterator_category = std::forward_iterator_tag;

    enum class Eof
    {
        EofMark
    };

    /**
     * Initializes a LexerIterator that purely marks the end of a lexically analyzed stream.
     */
    explicit LexerIterator(Eof);

    /**
     * Initializes a LexerIterator for a given source to be analyzed with given lexer definition .
     */
    LexerIterator(const LexerDef& ld, std::istream& source, TraceFn trace = TraceFn {});

    /**
     * Retrieves the default DFA machine that is used to recognize words.
     */
    Machine defaultMachine() const noexcept;

    /**
     * Sets the active deterministic finite automaton to use for recognizing words.
     *
     * @param machine the DFA machine to use for recognizing words.
     * @return the previous Machine state.
     */
    Machine setMachine(Machine machine);

    const TokenInfo& operator*() const noexcept { return currentToken_; }
    auto offset() const noexcept { return currentToken_.offset; }
    auto literal() const noexcept -> const std::string& { return currentToken_.literal; }
    auto token() const noexcept { return currentToken_.token; }
    auto name() const noexcept { return name(token()); }

    bool operator==(const LexerIterator& rhs) const noexcept;
    bool operator!=(const LexerIterator& rhs) const noexcept;

    LexerIterator& operator++();
    LexerIterator& operator++(int);

  private:
    void recognize();
    Token recognizeOne();

    // ---------------------------------------------------------------------------------
    // state helpers

    static constexpr StateId BadState = std::numeric_limits<StateId>::max();

    StateId getInitialState() const noexcept;
    bool isAcceptState(StateId state) const;

    /**
     * Retrieves the next state for given input state and input symbol.
     *
     * @param currentState the current State the DFA is in to.
     * @param inputSymbol the input symbol that is used for transitioning from current state to the next
     * state.
     * @returns the next state to transition to.
     */
    StateId delta(StateId currentState, Symbol inputSymbol) const;

    // ---------------------------------------------------------------------------------
    // stream helpers

    int currentChar() const noexcept { return currentChar_; }
    bool eof() const noexcept { return !source_->good(); }
    Symbol nextChar();
    void rollback();

    // ---------------------------------------------------------------------------------
    // debugging helpers

    template <typename... Args>
    void tracef(const char* msg, Args&&... args) const;

    const std::string& name(Token t) const;

    std::string toString(const std::deque<StateId>& stack);
    Token token(StateId s) const;
    static std::string stateName(StateId s);

  private:
    const LexerDef* def_ = nullptr;
    const TraceFn trace_;
    std::istream* source_ = nullptr;
    int eof_ = 0; // 0=No, 1=EOF_INIT, 2=EOF_FINAL

    TokenInfo currentToken_;
    Machine initialStateId_ = def_ ? defaultMachine() : Machine {};
    unsigned offset_ = 0;
    bool isBeginOfLine_ = true;
    int currentChar_ = -1;
    std::vector<int> buffered_;
};

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline Token token(const LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>& it)
{
    return it.token();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline size_t offset(const LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>& it)
{
    return it.offset();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline const std::string& literal(const LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>& it)
{
    return it.literal();
}

/**
 * @brief Holds a lexically analyzable stream of characters with a Lexer definition.
 */
template <typename Token = Tag,
          typename Machine = StateId,
          const bool RequiresBeginOfLine = true,
          const bool Trace = false>
class Lexable
{
  public:
    using TraceFn = std::function<void(const std::string&)>;
    using iterator = LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>;
    using value_type = TokenInfo<Token>;

    Lexable(const LexerDef& ld, std::istream& src, TraceFn trace = TraceFn {}):
        def_ { ld }, source_ { &src }, initialOffset_ { source_->tellg() }, trace_ { std::move(trace) }
    {
        if constexpr (!RequiresBeginOfLine)
            if (def_.containsBeginOfLineStates)
                throw std::invalid_argument {
                    "LexerDef contains a grammar that requires begin-of-line handling, but this Lexer has "
                    "begin-of-line support disabled."
                };
    }

    Lexable(const LexerDef& ld, const std::string& src, TraceFn trace = TraceFn {}):
        Lexable { ld, std::make_unique<std::stringstream>(src), std::move(trace) }
    {
    }

    Lexable(const LexerDef& ld, std::unique_ptr<std::istream>&& src, TraceFn trace = TraceFn {}):
        Lexable(ld, *src, std::move(trace))
    {
        ownedSource_ = std::move(src);
    }

    auto begin() const
    {
        source_->clear();
        source_->seekg(initialOffset_, std::ios::beg);
        return iterator { def_, *source_, trace_ };
    }

    auto end() const { return iterator { iterator::Eof::EofMark }; }

  private:
    const LexerDef& def_;
    std::unique_ptr<std::istream> ownedSource_;
    std::istream* source_;
    std::streamoff initialOffset_;
    TraceFn trace_;
};

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline auto begin(const Lexable<Token, Machine, RequiresBeginOfLine, Trace>& ls)
{
    return ls.begin();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline auto end(const Lexable<Token, Machine, RequiresBeginOfLine, Trace>& ls)
{
    return ls.end();
}

// {{{ LexerIterator: impl
template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::LexerIterator(Eof): eof_ { 2 }
{
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::LexerIterator(const LexerDef& ld,
                                                                         std::istream& source,
                                                                         TraceFn trace):
    def_ { &ld }, trace_ { trace }, source_ { &source }
{
    recognize();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
Machine LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::defaultMachine() const noexcept
{
    auto i = def_->initialStates.find("INITIAL");
    assert(i != def_->initialStates.end());
    return static_cast<Machine>(i->second);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
Machine LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::setMachine(Machine machine)
{
    return initialStateId_ = static_cast<StateId>(machine);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
bool LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::operator==(
    const LexerIterator& rhs) const noexcept
{
    return offset_ == rhs.offset_ || (eof_ == 2 && rhs.eof_ == 2);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
bool LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::operator!=(
    const LexerIterator& rhs) const noexcept
{
    return !(*this == rhs);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>& LexerIterator<Token,
                                                                         Machine,
                                                                         RequiresBeginOfLine,
                                                                         Trace>::operator++()
{
    if (eof())
        eof_++;

    recognize();
    return *this;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>& LexerIterator<Token,
                                                                         Machine,
                                                                         RequiresBeginOfLine,
                                                                         Trace>::operator++(int)
{
    if (eof())
        eof_++;

    recognize();
    return *this;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline void LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::recognize()
{
    for (;;)
        if (Token tag = recognizeOne(); static_cast<Tag>(tag) != IgnoreTag)
            return;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline Token LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::recognizeOne()
{
    // init
    currentToken_.offset = offset_;
    currentToken_.literal.clear();

    StateId state = getInitialState();
    std::deque<StateId> stack;
    stack.push_back(BadState);

    if constexpr (Trace)
        tracef("recognize: startState {}, offset {} {}",
               stateName(state),
               offset_,
               isBeginOfLine_ ? "BOL" : "no-BOL");

    // advance
    while (state != ErrorState)
    {
        Symbol ch = nextChar(); // one of: input character, ERROR or EOF
        currentToken_.literal.push_back(ch);

        // we do not stack.clear() stack if isAcceptState(state) as we need this information iff
        // lookahead is required. Otherwise we could clear here (for space savings)

        stack.push_back(state);
        state = delta(state, ch);
    }

    // backtrack to last (right-most) accept state
    while (state != BadState && !isAcceptState(state))
    {
        if constexpr (Trace)
            tracef("recognize: backtrack: current state {} {}; stack: {}",
                   stateName(state),
                   isAcceptState(state) ? "accepting" : "non-accepting",
                   toString(stack));

        state = stack.back();
        stack.pop_back();
        if (!currentToken_.literal.empty())
        {
            rollback();
            currentToken_.literal.resize(currentToken_.literal.size() - 1);
        }
    }

    // backtrack to right-most non-lookahead position in input stream
    if (auto i = def_->backtrackingStates.find(state); i != def_->backtrackingStates.end())
    {
        const StateId tmp = state;
        const StateId backtrackState = i->second;
        if constexpr (Trace)
            tracef("recognize: backtracking from {} to {}; stack: {}",
                   stateName(state),
                   stateName(backtrackState),
                   toString(stack));
        while (!stack.empty() && state != backtrackState)
        {
            state = stack.back();
            stack.pop_back();
            if constexpr (Trace)
                tracef("recognize: backtrack: state {}", stateName(state));
            if (!currentToken_.literal.empty())
            {
                rollback();
                currentToken_.literal.resize(currentToken_.literal.size() - 1);
            }
        }
        state = tmp;
    }

    if constexpr (Trace)
        tracef("recognize: final state {} {} {} {}-{} {} [currentChar: {}]",
               stateName(state),
               isAcceptState(state) ? "accepting" : "non-accepting",
               isAcceptState(state) ? name(token(state)) : std::string(),
               currentToken_.offset,
               offset_,
               quotedString(currentToken_.literal),
               quoted(currentChar_));

    if (!isAcceptState(state))
        throw LexerError { offset_ };

    auto i = def_->acceptStates.find(state);
    assert(i != def_->acceptStates.end() && "Accept state hit, but no tag assigned.");
    isBeginOfLine_ = currentToken_.literal.back() == '\n';

    return currentToken_.token = static_cast<Token>(i->second);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline StateId LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::getInitialState() const noexcept
{
    if constexpr (RequiresBeginOfLine)
        if (isBeginOfLine_ && def_->containsBeginOfLineStates)
            return static_cast<StateId>(initialStateId_) + 1;

    return static_cast<StateId>(initialStateId_);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline bool LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::isAcceptState(StateId id) const
{
    return def_->acceptStates.find(id) != def_->acceptStates.end();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
StateId LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::delta(StateId currentState,
                                                                         Symbol inputSymbol) const
{
    const StateId nextState = def_->transitions.apply(currentState, inputSymbol);
    if constexpr (Trace)
    {
        if (isAcceptState(nextState))
            tracef("recognize: state {:>4} --{:-^7}--> {:<6} (accepting: {})",
                   stateName(currentState),
                   prettySymbol(inputSymbol),
                   stateName(nextState),
                   name(token(nextState)));
        else
            tracef("recognize: state {:>4} --{:-^7}--> {:<6}",
                   stateName(currentState),
                   prettySymbol(inputSymbol),
                   stateName(nextState));
    }

    return nextState;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline Symbol LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::nextChar()
{
    if (!buffered_.empty())
    {
        int ch = buffered_.back();
        currentChar_ = ch;
        buffered_.resize(buffered_.size() - 1);
        if constexpr (Trace)
            tracef("Lexer:{}: advance '{}'", offset_, prettySymbol(ch));
        offset_++;
        return ch;
    }

    if (!source_->good())
    { // EOF or I/O error
        if constexpr (Trace)
            tracef("Lexer:{}: advance '{}'", offset_, "EOF");
        return Symbols::EndOfFile;
    }

    int ch = source_->get();
    if (ch < 0)
    {
        currentChar_ = Symbols::EndOfFile;
        offset_++;
        if constexpr (Trace)
            tracef("Lexer:{}: advance '{}'", offset_, prettySymbol(ch));
        return currentChar_;
    }

    currentChar_ = ch;
    if constexpr (Trace)
        tracef("Lexer:{}: advance '{}'", offset_, prettySymbol(ch));
    offset_++;
    return ch;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline void LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::rollback()
{
    currentChar_ = currentToken_.literal.back();
    if (currentToken_.literal.back() != -1)
    {
        offset_--;
        buffered_.push_back(currentToken_.literal.back());
    }
}

// =================================================================================

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
template <typename... Args>
inline void LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::tracef(const char* msg,
                                                                              Args&&... args) const
{
    if constexpr (Trace)
        if (trace_)
            trace_(fmt::format(msg, std::forward<Args>(args)...));
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
inline const std::string& LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::name(Token t) const
{
    auto i = def_->tagNames.find(static_cast<Tag>(t));
    assert(i != def_->tagNames.end());
    return i->second;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline std::string LexerIterator<Token, Machine, RequiresBeginOfLine, Debug>::toString(
    const std::deque<StateId>& stack)
{
    std::stringstream sstr;
    sstr << "{";
    int i = 0;
    for (const auto s: stack)
    {
        if (i)
            sstr << ",";
        sstr << stateName(s);
        i++;
    }

    sstr << "}";
    return sstr.str();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
Token LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>::token(StateId s) const
{
    auto i = def_->acceptStates.find(s);
    assert(i != def_->acceptStates.end());
    return static_cast<Token>(i->second);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline std::string LexerIterator<Token, Machine, RequiresBeginOfLine, Debug>::stateName(StateId s)
{
    switch (s)
    {
        case BadState: return "Bad";
        case ErrorState: return "Error";
        default: return fmt::format("n{}", std::to_string(s));
    }
}
// }}}

} // namespace regex_dfa

namespace std
{
template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
struct iterator_traits<regex_dfa::LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>>
{
    using iterator = regex_dfa::LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>;

    using difference_type = typename iterator::difference_type;
    using value_type = typename iterator::value_type;
    using pointer = typename iterator::pointer;
    using reference = typename iterator::reference;
    using iterator_category = typename iterator::iterator_category;
};
} // namespace std

namespace fmt
{
template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Trace>
struct formatter<regex_dfa::LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>>
{
    using TokenInfo = regex_dfa::TokenInfo<Token>;
    using LexerIterator = regex_dfa::LexerIterator<Token, Machine, RequiresBeginOfLine, Trace>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const LexerIterator& v, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{} ({})", v.literal(), v.name());
    }
};
} // namespace fmt
