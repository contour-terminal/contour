#pragma once

#include <crispy/overloaded.h>

#include <fmt/format.h>

#include <functional>
#include <string>
#include <variant>

namespace terminal
{

struct ParserErrorEvent
{
    std::string reason;
};

struct TraceInputEvent
{
    std::string message;
};

struct RawInputEvent
{
    std::string sequence;
};

struct RawOutputEvent
{
    std::string sequence;
};

struct InvalidOutputEvent
{
    std::string sequence;
    std::string reason;
};

struct UnsupportedOutputEvent
{
    std::string sequence;
};

struct TraceOutputEvent
{
    std::string sequence;
};

using LogEvent = std::variant<ParserErrorEvent,
                              TraceInputEvent,
                              RawInputEvent,
                              RawOutputEvent,
                              InvalidOutputEvent,
                              UnsupportedOutputEvent,
                              TraceOutputEvent>;

using Logger = std::function<void(LogEvent)>;

} // namespace terminal

namespace fmt
{
template <>
struct formatter<terminal::LogEvent>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const terminal::LogEvent& ev, FormatContext& ctx)
    {
        using namespace terminal;
        return std::visit(
            overloaded {
                [&](ParserErrorEvent const& v) { return format_to(ctx.out(), "Parser Error. {}", v.reason); },
                [&](TraceInputEvent const& v) { return format_to(ctx.out(), "Trace Input: {}", v.message); },
                [&](RawInputEvent const& v) { return format_to(ctx.out(), "Raw Input: \"{}\"", v.sequence); },
                [&](RawOutputEvent const& v) {
                    return format_to(ctx.out(), "Raw Output: \"{}\"", v.sequence);
                },
                [&](InvalidOutputEvent const& v) {
                    return format_to(ctx.out(), "Invalid output sequence: {}. {}", v.sequence, v.reason);
                },
                [&](UnsupportedOutputEvent const& v) {
                    return format_to(ctx.out(), "Unsupported output sequence: {}.", v.sequence);
                },
                [&](TraceOutputEvent const& v) {
                    return format_to(ctx.out(), "Trace output sequence: {}", v.sequence);
                },
            },
            ev);
    }
};
} // namespace fmt
