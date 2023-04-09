// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include <regex_dfa/SourceLocation.h>

namespace regex_dfa
{

class Report
{
  public:
    enum class Type
    {
        TokenError,
        SyntaxError,
        TypeError,
        Warning,
        LinkError
    };

    struct Message
    {
        Type type;
        SourceLocation sourceLocation;
        std::string text;

        Message(Type type, SourceLocation sloc, std::string text):
            type { type }, sourceLocation { std::move(sloc) }, text { std::move(text) }
        {
        }

        [[nodiscard]] std::string to_string() const;

        bool operator==(const Message& other) const noexcept;
        bool operator!=(const Message& other) const noexcept { return !(*this == other); }
    };

    using MessageList = std::vector<Message>;
    using Reporter = std::function<void(Message)>;

    explicit Report(Reporter reporter): onReport_ { std::move(reporter) } {}

    template <typename... Args>
    void tokenError(const SourceLocation& sloc, const std::string& f, Args&&... args)
    {
        report(Type::TokenError, sloc, fmt::format(f, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void syntaxError(const SourceLocation& sloc, const std::string& f, Args&&... args)
    {
        report(Type::SyntaxError, sloc, fmt::format(f, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void typeError(const SourceLocation& sloc, const std::string& f, Args&&... args)
    {
        report(Type::TypeError, sloc, fmt::format(f, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warning(const SourceLocation& sloc, const std::string& f, Args&&... args)
    {
        report(Type::Warning, sloc, fmt::format(f, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void linkError(const std::string& f, Args&&... args)
    {
        report(Type::LinkError, SourceLocation {}, fmt::format(f, std::forward<Args>(args)...));
    }

    void report(Type type, SourceLocation sloc, std::string text)
    {
        if (type != Type::Warning)
            errorCount_++;

        if (onReport_)
        {
            onReport_(Message(type, std::move(sloc), std::move(text)));
        }
    }

    [[nodiscard]] bool containsFailures() const noexcept { return errorCount_ != 0; }

  private:
    size_t errorCount_ = 0;
    Reporter onReport_;
};

class ConsoleReport: public Report
{
  public:
    ConsoleReport(): Report(std::bind(&ConsoleReport::onMessage, this, std::placeholders::_1)) {}

  private:
    void onMessage(Message&& msg);
};

class BufferedReport: public Report
{
  public:
    BufferedReport(): Report(std::bind(&BufferedReport::onMessage, this, std::placeholders::_1)), messages_ {}
    {
    }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] const MessageList& messages() const noexcept { return messages_; }

    void clear();
    [[nodiscard]] size_t size() const noexcept { return messages_.size(); }
    [[nodiscard]] const Message& operator[](size_t i) const { return messages_[i]; }

    using iterator = MessageList::iterator;
    using const_iterator = MessageList::const_iterator;

    [[nodiscard]] iterator begin() noexcept { return messages_.begin(); }
    [[nodiscard]] iterator end() noexcept { return messages_.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return messages_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return messages_.end(); }

    [[nodiscard]] bool contains(const Message& m) const noexcept;

    [[nodiscard]] bool operator==(const BufferedReport& other) const noexcept;
    [[nodiscard]] bool operator!=(const BufferedReport& other) const noexcept { return !(*this == other); }

  private:
    void onMessage(Message&& msg);

  private:
    MessageList messages_;
};

std::ostream& operator<<(std::ostream& os, const BufferedReport& report);

using DifferenceReport = std::pair<Report::MessageList, Report::MessageList>;

DifferenceReport difference(const BufferedReport& first, const BufferedReport& second);

} // namespace regex_dfa

namespace fmt
{
template <>
struct formatter<regex_dfa::Report::Type>: formatter<std::string_view>
{
    using Type = regex_dfa::Report::Type;

    static std::string_view to_stringview(Type t)
    {
        switch (t)
        {
            case Type::TokenError: return "TokenError";
            case Type::SyntaxError: return "SyntaxError";
            case Type::TypeError: return "TypeError";
            case Type::Warning: return "Warning";
            case Type::LinkError: return "LinkError";
            default: return "???";
        }
    }

    template <typename FormatContext>
    constexpr auto format(Type v, FormatContext& ctx)
    {
        return formatter<std::string_view>::format(to_stringview(v), ctx);
    }
};
} // namespace fmt

namespace fmt
{
template <>
struct formatter<regex_dfa::SourceLocation>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const regex_dfa::SourceLocation& sloc, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{} ({}-{})", sloc.filename, sloc.offset, sloc.offset + sloc.count);
    }
};
} // namespace fmt

namespace fmt
{
template <>
struct formatter<regex_dfa::Report::Message>
{
    using Message = regex_dfa::Report::Message;

    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(const Message& v, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}", v.to_string());
    }
};
} // namespace fmt
