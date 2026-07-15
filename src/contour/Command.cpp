// SPDX-License-Identifier: Apache-2.0
#include <contour/AsciiText.h>
#include <contour/Command.h>

#include <vtbackend/Color.h>

#include <crispy/escape.h>
#include <crispy/utils.h>

#include <format>
#include <ranges>
#include <string>
#include <string_view>

namespace contour
{

namespace
{
    /// True when @p ch begins a new word in an upper-camel-case identifier.
    ///
    /// Two ways to begin one: a capital right after a lower-case letter ("SplitVertical" breaks before
    /// 'V'), or a capital that ENDS a run of capitals by being followed by a lower-case letter
    /// ("VTMode" breaks before 'M'). A capital in the middle of a run does neither, which is what holds
    /// an acronym together ("ScreenshotVT" stays "Screenshot VT" and does not become "Screenshot V T").
    ///
    /// @param previous The character before @p ch, or '\0' at the start.
    /// @param ch       The character being considered.
    /// @param next     The character after @p ch, or '\0' at the end.
    [[nodiscard]] constexpr bool startsWord(char previous, char ch, char next) noexcept
    {
        if (!ascii::isUpper(ch))
            return false;
        if (ascii::isLower(previous))
            return true;
        return ascii::isUpper(previous) && ascii::isLower(next);
    }
} // namespace

std::string splitCamelCase(std::string_view identifier)
{
    auto result = std::string {};
    result.reserve(identifier.size() + 8);

    for (auto const index: std::views::iota(std::size_t { 0 }, identifier.size()))
    {
        auto const previous = index > 0 ? identifier[index - 1] : '\0';
        auto const next = index + 1 < identifier.size() ? identifier[index + 1] : '\0';
        if (index > 0 && startsWord(previous, identifier[index], next))
            result += ' ';
        result += identifier[index];
    }

    return result;
}

CommandArguments commandArguments(actions::Action const& action)
{
    // One visitor, both facets (see CommandArguments). Every action carrying an argument names it here;
    // every action that does not falls into the generic tail and gets none — so its id is just its name
    // and its title is just its words.
    //
    // NB: this cannot delegate to std::formatter<Action>. That formatter renders an action for a LOG
    // (PasteClipboard prints as "PasteClipboard" whether or not it strips), and an identity that drops
    // an argument would collapse two commands that do different things onto one row.
    using namespace actions;
    return std::visit(
        crispy::overloaded {
            [](ChangeProfile const& a) -> CommandArguments {
                return { .id = a.name, .title = std::format(": {}", a.name) };
            },
            [](CreateSelection const& a) -> CommandArguments {
                return { .id = a.delimiters, .title = std::format(": {}", a.delimiters) };
            },
            [](HintMode const& a) -> CommandArguments {
                return { .id = std::format("{}:{}", a.patterns, a.hintAction),
                         .title = std::format(": {} ({})", a.patterns, a.hintAction) };
            },
            [](SendChars const& a) -> CommandArguments {
                return { .id = a.chars, .title = std::format(": {}", crispy::escape(a.chars)) };
            },
            [](WriteScreen const& a) -> CommandArguments {
                return { .id = a.chars, .title = std::format(": {}", crispy::escape(a.chars)) };
            },
            [](MoveTabTo const& a) -> CommandArguments {
                return { .id = std::format("{}", a.position), .title = std::format(" {}", a.position) };
            },
            [](SwitchToTab const& a) -> CommandArguments {
                return { .id = std::format("{}", a.position), .title = std::format(" {}", a.position) };
            },
            [](ResizePane const& a) -> CommandArguments {
                return { .id = std::format("{}:{}", a.direction, a.percent),
                         .title = std::format(" {} ({}%)", a.direction, a.percent) };
            },
            [](LaunchLayout const& a) -> CommandArguments {
                return { .id = a.name, .title = std::format(": {}", a.name) };
            },
            [](SetTabColor const& a) -> CommandArguments {
                // Colorless is the DEFAULT (it opens the picker), so it carries no arguments: the row
                // keeps the plain "SetTabColor" id, and the palette can offer it straight from the
                // catalog. A bound instance that names a color is a different command, and says so.
                if (!a.color)
                    return {};
                auto hex = vtbackend::formatColor(*a.color);
                auto title = std::format(": {}", hex);
                return { .id = std::move(hex), .title = std::move(title) };
            },
            [](SaveLayout const& a) -> CommandArguments {
                return { .id = a.name, .title = std::format(": {}", a.name) };
            },
            // The defaulted-argument actions. Their argument only enters the identity when it is NOT the
            // default, so the plain "PasteClipboard" id — and any MRU entry already written under it —
            // stays stable, while "PasteClipboard:strip" remains a distinct command.
            [](CopySelection const& a) -> CommandArguments {
                if (a.format == CopyFormat::Text)
                    return {};
                return { .id = std::format("{}", a.format), .title = std::format(" as {}", a.format) };
            },
            [](PasteClipboard const& a) -> CommandArguments {
                if (!a.strip)
                    return {};
                return { .id = "strip", .title = " (strip whitespace)" };
            },
            [](PasteSelection const& a) -> CommandArguments {
                if (!a.evaluateInShell)
                    return {};
                return { .id = "evaluate", .title = " (evaluate in shell)" };
            },
            [](NewTerminal const& a) -> CommandArguments {
                if (!a.profileName.has_value())
                    return {};
                return { .id = *a.profileName, .title = std::format(": {}", *a.profileName) };
            },
            [](ReloadConfig const& a) -> CommandArguments {
                if (!a.profileName.has_value())
                    return {};
                return { .id = *a.profileName, .title = std::format(": {}", *a.profileName) };
            },
            [](auto const&) -> CommandArguments { return {}; },
        },
        action);
}

std::string commandId(actions::Action const& action)
{
    auto const arguments = commandArguments(action);
    auto const actionName = actions::name(action);

    if (arguments.id.empty())
        return std::string(actionName);

    return std::format("{}:{}", actionName, arguments.id);
}

std::string commandTitle(actions::Action const& action)
{
    return splitCamelCase(actions::name(action)) + commandArguments(action).title;
}

} // namespace contour
