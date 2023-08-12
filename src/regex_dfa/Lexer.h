// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/LexerDef.h>

#include <fmt/format.h>

#include <cassert>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace regex_dfa
{

template <typename Token = Tag>
struct TokenInfo
{
    Token token;
    std::string literal;
    size_t offset;

    operator Token() const noexcept { return token; }

    friend bool operator==(const TokenInfo<Token>& a, Token b) noexcept { return a.token == b; }
    friend bool operator!=(const TokenInfo<Token>& a, Token b) noexcept { return a.token != b; }
    friend bool operator==(Token a, const TokenInfo<Token>& b) noexcept { return b == a; }
    friend bool operator!=(Token a, const TokenInfo<Token>& b) noexcept { return b != a; }
};

template <typename Token>
[[nodiscard]] inline Token token(const TokenInfo<Token>& it)
{
    return it.token;
}

template <typename Token>
[[nodiscard]] inline size_t offset(const TokenInfo<Token>& it)
{
    return it.offset;
}

template <typename Token>
[[nodiscard]] inline const std::string& literal(const TokenInfo<Token>& it)
{
    return it.literal;
}

template <typename Token>
[[nodiscard]] inline const std::string& to_string(const TokenInfo<Token>& info) noexcept
{
    return info.literal;
}

/**
 * Lexer API for recognizing words.
 */
template <typename Token = Tag,
          typename Machine = StateId,
          const bool RequiresBeginOfLine = true,
          const bool Debug = false>
class Lexer
{
  public:
    using value_type = Token;
    using DebugLogger = std::function<void(const std::string&)>;
    using TokenInfo = regex_dfa::TokenInfo<Token>;

    //! Constructs the Lexer with the given information table.
    explicit Lexer(const LexerDef& info, DebugLogger logger = DebugLogger {});

    //! Constructs the Lexer with the given information table and input stream.
    Lexer(const LexerDef& info, std::unique_ptr<std::istream> input, DebugLogger logger = DebugLogger {});

    //! Constructs the Lexer with the given information table and input stream.
    Lexer(const LexerDef& info, std::istream& input, DebugLogger logger = DebugLogger {});

    //! Constructs the Lexer with the given information table and input stream.
    Lexer(const LexerDef& info, std::string input, DebugLogger logger = DebugLogger {});

    /**
     * Open given input stream.
     */
    void reset(std::unique_ptr<std::istream> input);
    void reset(const std::string& input);

    /**
     * Recognizes one token (ignored patterns are skipped).
     */
    [[nodiscard]] TokenInfo recognize();

    /**
     * Recognizes one token, regardless of it is to be ignored or not.
     */
    [[nodiscard]] Token recognizeOne();

    //! the underlying word of the currently recognized token
    [[nodiscard]] const std::string& word() const { return word_; }

    //! @returns the absolute offset of the file the lexer is currently reading from.
    [[nodiscard]] std::pair<unsigned, unsigned> offset() const noexcept
    {
        return std::make_pair(oldOffset_, offset_);
    }

    //! @returns the last recognized token.
    [[nodiscard]] Token token() const noexcept { return token_; }

    //! @returns the name of the current token.
    [[nodiscard]] const std::string& name() const { return name(token_); }

    //! @returns the name of the token represented by Token @p t.
    [[nodiscard]] const std::string& name(Token t) const
    {
        auto i = def_.tagNames.find(static_cast<Tag>(t));
        assert(i != def_.tagNames.end());
        return i->second;
    }

    /**
     * Retrieves the next state for given input state and input symbol.
     *
     * @param currentState the current State the DFA is in to.
     * @param inputSymbol the input symbol that is used for transitioning from current state to the next
     * state.
     * @returns the next state to transition to.
     */
    [[nodiscard]] inline StateId delta(StateId currentState, Symbol inputSymbol) const;

    /**
     * Sets the active deterministic finite automaton to use for recognizing words.
     *
     * @param machine the DFA machine to use for recognizing words.
     */
    Machine setMachine(Machine machine)
    {
        auto const oldMachine = initialStateId_;
        // since Machine is a 1:1 mapping into the State's ID, we can simply cast here.
        initialStateId_ = static_cast<StateId>(machine);
        return oldMachine;
    }

    /**
     * Retrieves the default DFA machine that is used to recognize words.
     */
    [[nodiscard]] Machine defaultMachine() const
    {
        auto i = def_.initialStates.find("INITIAL");
        assert(i != def_.initialStates.end());
        return static_cast<Machine>(i->second);
    }

    /**
     * Runtime exception that is getting thrown when a word could not be recognized.
     */
    struct LexerError: public std::runtime_error
    {
        LexerError(unsigned int offset):
            std::runtime_error { fmt::format("[{}] Failed to lexically recognize a word.", offset) },
            offset { offset }
        {
        }

        unsigned int offset;
    };

    struct iterator // NOLINT(readability-identifier-naming)
    {
        Lexer& lx;
        int end;
        TokenInfo info;

        const TokenInfo& operator*() const { return info; }

        iterator& operator++()
        {
            if (lx.eof())
                ++end;

            info = lx.recognize();

            return *this;
        }

        iterator& operator++(int) { return ++*this; }
        bool operator==(const iterator& rhs) const noexcept { return end == rhs.end; }
        bool operator!=(const iterator& rhs) const noexcept { return !(*this == rhs); }
    };

    iterator begin()
    {
        const Token t = recognize();
        return iterator { *this, 0, TokenInfo { t, word() } };
    }

    iterator end() { return iterator { *this, 2, TokenInfo { 0, "" } }; }

    [[nodiscard]] bool eof() const { return !stream_->good(); }

    [[nodiscard]] size_t fileSize() const noexcept { return fileSize_; }

  private:
    template <typename... Args>
    inline void debugf(const char* msg, Args... args) const
    {
        if constexpr (Debug)
            if (debug_)
                debug_(fmt::format(msg, args...));
    }

    [[nodiscard]] Symbol nextChar();
    void rollback();
    [[nodiscard]] StateId getInitialState() const noexcept;
    [[nodiscard]] bool isAcceptState(StateId state) const noexcept;
    [[nodiscard]] static std::string stateName(StateId s, std::string_view n = "n");
    static constexpr StateId BadState = 101010;
    [[nodiscard]] std::string toString(const std::deque<StateId>& stack);

    [[nodiscard]] int currentChar() const noexcept { return currentChar_; }

    [[nodiscard]] Token token(StateId s) const
    {
        auto i = def_.acceptStates.find(s);
        assert(i != def_.acceptStates.end());
        return static_cast<Token>(i->second);
    }

    [[nodiscard]] size_t getFileSize();

  private:
    const LexerDef& def_;
    const DebugLogger debug_;

    Machine initialStateId_;
    std::string word_;
    std::unique_ptr<std::istream> ownedStream_;
    std::istream* stream_;
    std::vector<int> buffered_;
    unsigned oldOffset_;
    unsigned offset_;
    size_t fileSize_; // cache
    bool isBeginOfLine_;
    int currentChar_;
    Token token_;
};

template <typename Token = Tag,
          typename Machine = StateId,
          const bool RequiresBeginOfLine = true,
          const bool Debug = false>
inline const std::string& to_string(
    const typename Lexer<Token, Machine, RequiresBeginOfLine, Debug>::iterator& it) noexcept
{
    return it.info.literal;
}

} // namespace regex_dfa

namespace fmt
{
template <typename Token>
struct formatter<regex_dfa::TokenInfo<Token>>
{
    using TokenInfo = regex_dfa::TokenInfo<Token>;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const TokenInfo& v, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", v.literal);
    }
};
} // namespace fmt
#include <regex_dfa/Lexer-inl.h>
