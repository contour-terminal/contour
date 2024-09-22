#pragma once

#include <crispy/overloaded.h>

#include <format>
#include <functional>
#include <string>
#include <variant>

namespace vtbackend
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

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::LogEvent>
{
    auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(const vtbackend::LogEvent& ev, auto& ctx) const
    {
        using namespace vtbackend;
        return std::visit(
            overloaded {
                [&](ParserErrorEvent const& v) {
                    return std::format_to(ctx.out(), "Parser Error. {}", v.reason);
                },
                [&](TraceInputEvent const& v) {
                    return std::format_to(ctx.out(), "Trace Input: {}", v.message);
                },
                [&](RawInputEvent const& v) {
                    return std::format_to(ctx.out(), "Raw Input: \"{}\"", v.sequence);
                },
                [&](RawOutputEvent const& v) {
                    return std::format_to(ctx.out(), "Raw Output: \"{}\"", v.sequence);
                },
                [&](InvalidOutputEvent const& v) {
                    return std::format_to(ctx.out(), "Invalid output sequence: {}. {}", v.sequence, v.reason);
                },
                [&](UnsupportedOutputEvent const& v) {
                    return std::format_to(ctx.out(), "Unsupported output sequence: {}.", v.sequence);
                },
                [&](TraceOutputEvent const& v) {
                    return std::format_to(ctx.out(), "Trace output sequence: {}", v.sequence);
                },
            },
            ev);
    }
};
