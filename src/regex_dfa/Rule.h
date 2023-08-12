// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/LexerDef.h> // IgnoreTag
#include <regex_dfa/RegExpr.h>
#include <regex_dfa/RegExprParser.h>
#include <regex_dfa/State.h> // Tag

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace regex_dfa
{

struct Rule
{
    unsigned int line;
    unsigned int column;
    Tag tag;
    std::vector<std::string> conditions;
    std::string name;
    std::string pattern;
    std::unique_ptr<RegExpr> regexpr = nullptr;

    [[nodiscard]] bool isIgnored() const noexcept { return tag == IgnoreTag; }

    [[nodiscard]] Rule clone() const
    {
        return regexpr ? Rule { line,
                                column,
                                tag,
                                conditions,
                                name,
                                pattern,
                                std::make_unique<RegExpr>(RegExprParser {}.parse(pattern, line, column)) }
                       : Rule { line, column, tag, conditions, name, pattern, nullptr };
    }

    Rule() = default;

    Rule(unsigned line,
         unsigned column,
         Tag tag,
         std::vector<std::string> conditions,
         std::string name,
         std::string pattern,
         std::unique_ptr<RegExpr> regexpr = nullptr):
        line { line },
        column { column },
        tag { tag },
        conditions { std::move(conditions) },
        name { std::move(name) },
        pattern { std::move(pattern) },
        regexpr { std::move(regexpr) }
    {
    }

    Rule(const Rule& v):
        line { v.line },
        column { v.column },
        tag { v.tag },
        conditions { v.conditions },
        name { v.name },
        pattern { v.pattern },
        regexpr { v.regexpr ? std::make_unique<RegExpr>(RegExprParser {}.parse(pattern, line, column))
                            : nullptr }
    {
    }

    Rule& operator=(const Rule& v)
    {
        line = v.line;
        column = v.column;
        tag = v.tag;
        conditions = v.conditions;
        name = v.name;
        pattern = v.pattern;
        regexpr =
            v.regexpr ? std::make_unique<RegExpr>(RegExprParser {}.parse(pattern, line, column)) : nullptr;
        return *this;
    }

    bool operator<(const Rule& rhs) const noexcept { return tag < rhs.tag; }
    bool operator<=(const Rule& rhs) const noexcept { return tag <= rhs.tag; }
    bool operator==(const Rule& rhs) const noexcept { return tag == rhs.tag; }
    bool operator!=(const Rule& rhs) const noexcept { return tag != rhs.tag; }
    bool operator>=(const Rule& rhs) const noexcept { return tag >= rhs.tag; }
    bool operator>(const Rule& rhs) const noexcept { return tag > rhs.tag; }
};

using RuleList = std::vector<Rule>;

inline bool ruleContainsBeginOfLine(const Rule& r)
{
    return containsBeginOfLine(*r.regexpr);
}

} // namespace regex_dfa

namespace fmt
{
template <>
struct formatter<regex_dfa::Rule>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const regex_dfa::Rule& v, FormatContext& ctx)
    {
        if (!v.conditions.empty())
        {
            fmt::format_to(ctx.out(), "<");
            for (size_t i = 0; i < v.conditions.size(); ++i)
                if (i != 0)
                    fmt::format_to(ctx.out(), ", {}", v.conditions[i]);
                else
                    fmt::format_to(ctx.out(), "{}", v.conditions[i]);
            fmt::format_to(ctx.out(), ">");
        }
        if (v.tag == regex_dfa::IgnoreTag)
            return fmt::format_to(ctx.out(), "{}({}) ::= {}", v.name, "ignore", v.pattern);
        else
            return fmt::format_to(ctx.out(), "{}({}) ::= {}", v.name, v.tag, v.pattern);
    }
};
} // namespace fmt
