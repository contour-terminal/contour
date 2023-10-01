#pragma once

#include <crispy/overloaded.h>

#include <fmt/format.h>

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
struct fmt::formatter<vtbackend::LogEvent>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(const vtbackend::LogEvent& ev, format_context& ctx) -> format_context::iterator
    {
        using namespace vtbackend;
        return std::visit(
            overloaded {
                [&](ParserErrorEvent const& v) {
                    return fmt::format_to(ctx.out(), "Parser Error. {}", v.reason);
                },
                [&](TraceInputEvent const& v) {
                    return fmt::format_to(ctx.out(), "Trace Input: {}", v.message);
                },
                [&](RawInputEvent const& v) {
                    return fmt::format_to(ctx.out(), "Raw Input: \"{}\"", v.sequence);
                },
                [&](RawOutputEvent const& v) {
                    return fmt::format_to(ctx.out(), "Raw Output: \"{}\"", v.sequence);
                },
                [&](InvalidOutputEvent const& v) {
                    return fmt::format_to(ctx.out(), "Invalid output sequence: {}. {}", v.sequence, v.reason);
                },
                [&](UnsupportedOutputEvent const& v) {
                    return fmt::format_to(ctx.out(), "Unsupported output sequence: {}.", v.sequence);
                },
                [&](TraceOutputEvent const& v) {
                    return fmt::format_to(ctx.out(), "Trace output sequence: {}", v.sequence);
                },
            },
            ev);
    }
};
