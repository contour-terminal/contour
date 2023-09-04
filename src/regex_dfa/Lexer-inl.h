// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Lexer.h>

#include <algorithm>
#include <deque>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace regex_dfa
{

static inline std::string quotedString(const std::string& s)
{
    std::stringstream sstr;
    sstr << std::quoted(s);
    return sstr.str();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline Lexer<Token, Machine, RequiresBeginOfLine, Debug>::Lexer(const LexerDef& info, DebugLogger logger):
    def_ { info },
    debug_ { logger },
    initialStateId_ { defaultMachine() },
    word_ {},
    ownedStream_ {},
    stream_ { nullptr },
    oldOffset_ { 0 },
    offset_ { 0 },
    fileSize_ { 0 },
    isBeginOfLine_ { true },
    token_ { 0 }
{
    if constexpr (!RequiresBeginOfLine)
        if (def_.containsBeginOfLineStates)
            throw std::invalid_argument {
                "LexerDef contains a grammar that requires begin-of-line handling, but this Lexer has "
                "begin-of-line support disabled."
            };
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline Lexer<Token, Machine, RequiresBeginOfLine, Debug>::Lexer(const LexerDef& info,
                                                                std::unique_ptr<std::istream> stream,
                                                                DebugLogger logger):
    Lexer { info, std::move(logger) }
{
    reset(std::move(stream));
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline Lexer<Token, Machine, RequiresBeginOfLine, Debug>::Lexer(const LexerDef& info,
                                                                std::istream& stream,
                                                                DebugLogger logger):
    Lexer { info, std::move(logger) }
{
    stream_ = &stream;
    fileSize_ = getFileSize();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline Lexer<Token, Machine, RequiresBeginOfLine, Debug>::Lexer(const LexerDef& info,
                                                                std::string input,
                                                                DebugLogger logger):
    Lexer { info, std::move(logger) }
{
    reset(std::make_unique<std::stringstream>(std::move(input)));
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline void Lexer<Token, Machine, RequiresBeginOfLine, Debug>::reset(std::unique_ptr<std::istream> stream)
{
    ownedStream_ = std::move(stream);
    stream_ = ownedStream_.get();
    oldOffset_ = 0;
    offset_ = 0;
    isBeginOfLine_ = true;
    fileSize_ = getFileSize();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline void Lexer<Token, Machine, RequiresBeginOfLine, Debug>::reset(const std::string& text)
{
    reset(std::make_unique<std::stringstream>(text));
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline size_t Lexer<Token, Machine, RequiresBeginOfLine, Debug>::getFileSize()
{
    std::streamoff oldpos = stream_->tellg();
    stream_->seekg(0, stream_->end);

    std::streamoff theSize = stream_->tellg();
    stream_->seekg(oldpos, stream_->beg);

    return static_cast<size_t>(theSize);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline std::string Lexer<Token, Machine, RequiresBeginOfLine, Debug>::stateName(StateId s, std::string_view n)
{
    switch (s)
    {
        case BadState: return "Bad";
        case ErrorState: return "Error";
        default: return fmt::format("{}{}", n, std::to_string(s));
    }
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline std::string Lexer<Token, Machine, RequiresBeginOfLine, Debug>::toString(
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

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline auto Lexer<Token, Machine, RequiresBeginOfLine, Debug>::recognize() -> TokenInfo
{
    for (;;)
        if (Token tag = recognizeOne(); static_cast<Tag>(tag) != IgnoreTag)
            return TokenInfo { tag, word_, oldOffset_ };
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline StateId Lexer<Token, Machine, RequiresBeginOfLine, Debug>::getInitialState() const noexcept
{
    if constexpr (RequiresBeginOfLine)
    {
        if (isBeginOfLine_ && def_.containsBeginOfLineStates)
        {
            return static_cast<StateId>(initialStateId_) + 1;
        }
    }

    return static_cast<StateId>(initialStateId_);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline Token Lexer<Token, Machine, RequiresBeginOfLine, Debug>::recognizeOne()
{
    // init
    oldOffset_ = offset_;
    word_.clear();
    StateId state = getInitialState();
    std::deque<StateId> stack;
    stack.push_back(BadState);

    if constexpr (Debug)
        debugf("recognize: startState {}, offset {} {}",
               stateName(state),
               offset_,
               isBeginOfLine_ ? "BOL" : "no-BOL");

    // advance
    while (state != ErrorState)
    {
        Symbol ch = nextChar(); // one of: input character, ERROR or EOF
        word_.push_back(ch);

        // we do not stack.clear() stack if isAcceptState(state) as we need this information iff
        // lookahead is required. Otherwise we could clear here (for space savings)

        stack.push_back(state);
        state = delta(state, ch);
    }

    // backtrack to last (right-most) accept state
    while (state != BadState && !isAcceptState(state))
    {
        if constexpr (Debug)
            debugf("recognize: backtrack: current state {} {}; stack: {}",
                   stateName(state),
                   isAcceptState(state) ? "accepting" : "non-accepting",
                   toString(stack));

        state = stack.back();
        stack.pop_back();
        if (!word_.empty())
        {
            rollback();
            word_.resize(word_.size() - 1);
        }
    }

    // backtrack to right-most non-lookahead position in input stream
    if (auto i = def_.backtrackingStates.find(state); i != def_.backtrackingStates.end())
    {
        const StateId tmp = state;
        const StateId backtrackState = i->second;
        if constexpr (Debug)
            debugf("recognize: backtracking from {} to {}; stack: {}",
                   stateName(state),
                   stateName(backtrackState),
                   toString(stack));
        while (!stack.empty() && state != backtrackState)
        {
            state = stack.back();
            stack.pop_back();
            if constexpr (Debug)
                debugf("recognize: backtrack: state {}", stateName(state));
            if (!word_.empty())
            {
                rollback();
                word_.resize(word_.size() - 1);
            }
        }
        state = tmp;
    }

    if constexpr (Debug)
        debugf("recognize: final state {} {} {} {}-{} {} [currentChar: {}]",
               stateName(state),
               isAcceptState(state) ? "accepting" : "non-accepting",
               isAcceptState(state) ? name(token(state)) : std::string(),
               oldOffset_,
               offset_,
               quotedString(word_),
               prettySymbol(currentChar_));

    if (!isAcceptState(state))
        throw LexerError { offset_ };

    auto i = def_.acceptStates.find(state);
    assert(i != def_.acceptStates.end() && "Accept state hit, but no tag assigned.");
    isBeginOfLine_ = word_.back() == '\n';
    return token_ = static_cast<Token>(i->second);
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline StateId Lexer<Token, Machine, RequiresBeginOfLine, Debug>::delta(StateId currentState,
                                                                        Symbol inputSymbol) const
{
    const StateId nextState = def_.transitions.apply(currentState, inputSymbol);
    if constexpr (Debug)
    {
        if (isAcceptState(nextState))
        {
            debugf("recognize: state {:>4} --{:-^7}--> {:<6} (accepting: {})",
                   stateName(currentState),
                   prettySymbol(inputSymbol),
                   stateName(nextState),
                   name(token(nextState)));
        }
        else
        {
            debugf("recognize: state {:>4} --{:-^7}--> {:<6}",
                   stateName(currentState),
                   prettySymbol(inputSymbol),
                   stateName(nextState));
        }
    }

    return nextState;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline bool Lexer<Token, Machine, RequiresBeginOfLine, Debug>::isAcceptState(StateId state) const noexcept
{
    return def_.acceptStates.find(state) != def_.acceptStates.end();
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline Symbol Lexer<Token, Machine, RequiresBeginOfLine, Debug>::nextChar()
{
    if (!buffered_.empty())
    {
        int ch = buffered_.back();
        currentChar_ = ch;
        buffered_.resize(buffered_.size() - 1);
        if constexpr (Debug)
            debugf("Lexer:{}: advance '{}'", offset_, prettySymbol(ch));
        offset_++;
        return ch;
    }

    if (!stream_->good())
    { // EOF or I/O error
        if constexpr (Debug)
            debugf("Lexer:{}: advance '{}'", offset_, "EOF");
        return Symbols::EndOfFile;
    }

    int ch = stream_->get();
    if (ch < 0)
    {
        currentChar_ = Symbols::EndOfFile;
        offset_++;
        if constexpr (Debug)
            debugf("Lexer:{}: advance '{}'", offset_, prettySymbol(ch));
        return currentChar_;
    }

    currentChar_ = ch;
    if constexpr (Debug)
        debugf("Lexer:{}: advance '{}'", offset_, prettySymbol(ch));
    offset_++;
    return ch;
}

template <typename Token, typename Machine, const bool RequiresBeginOfLine, const bool Debug>
inline void Lexer<Token, Machine, RequiresBeginOfLine, Debug>::rollback()
{
    currentChar_ = word_.back();
    if (word_.back() != -1)
    {
        offset_--;
        buffered_.push_back(word_.back());
    }
}

} // namespace regex_dfa
