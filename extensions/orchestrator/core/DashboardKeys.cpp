// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/DashboardKeys.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    constexpr auto Keys = std::array {
        DashboardKey { .name = "enter"sv, .character = '\r' },
        DashboardKey { .name = "newline"sv, .character = '\n' },
        DashboardKey { .name = "tab"sv, .character = '\t' },
        DashboardKey { .name = "d"sv, .character = 'd' },
        DashboardKey { .name = "e"sv, .character = 'e' },
        DashboardKey { .name = "b"sv, .character = 'b' },
        DashboardKey { .name = "t"sv, .character = 't' },
        DashboardKey { .name = "x"sv, .character = 'x' },
        DashboardKey { .name = "z"sv, .character = 'z' },
        DashboardKey { .name = "n"sv, .character = 'n' },
        DashboardKey { .name = "I"sv, .character = 'I' },
        DashboardKey { .name = "m"sv, .character = 'm' },
        DashboardKey { .name = "/"sv, .character = '/' },
        DashboardKey { .name = "?"sv, .character = '?' },
        DashboardKey { .name = "j"sv, .character = 'j' },
        DashboardKey { .name = "k"sv, .character = 'k' },
        DashboardKey { .name = "R"sv, .character = 'R' },
        DashboardKey { .name = "del"sv, .character = '\0' },
        DashboardKey { .name = "home"sv, .character = '\0' },
        DashboardKey { .name = "end"sv, .character = '\0' },
        DashboardKey { .name = "esc"sv, .character = '\0' },
        DashboardKey { .name = "up"sv, .character = '\0' },
        DashboardKey { .name = "down"sv, .character = '\0' },
        DashboardKey { .name = "left"sv, .character = '\0' },
        DashboardKey { .name = "right"sv, .character = '\0' },
        DashboardKey { .name = "pgup"sv, .character = '\0' },
        DashboardKey { .name = "pgdn"sv, .character = '\0' },
    };

    constexpr auto MessagePassKeys = std::array {
        MessagePassKey { .name = "pgup"sv, .does = "scroll a page up"sv },
        MessagePassKey { .name = "pgdn"sv, .does = "scroll a page down"sv },
        MessagePassKey { .name = "end"sv, .does = "follow the transcript again"sv },
        MessagePassKey { .name = "left"sv, .does = "turn to the previous transcript page"sv },
        MessagePassKey { .name = "right"sv, .does = "turn to the next transcript page"sv },
    };

    constexpr auto Navigating = NavContext::Navigating;
    constexpr auto Typing = NavContext::Typing;

    /// The command keys but the message box's pass-through ones, which MessagePassKeys adds.
    constexpr auto OwnCommandKeys = std::array {
        CommandKey { .name = "enter"sv, .does = "start or open the selected row"sv, .context = Navigating },
        CommandKey { .name = "esc"sv, .does = "go back"sv, .context = Navigating },
        CommandKey { .name = "tab"sv, .does = "move between panes and fields"sv, .context = Navigating },
        CommandKey { .name = "del"sv, .does = "dismiss a finished run"sv, .context = Navigating },
        CommandKey { .name = "pgup"sv, .does = "scroll a page up"sv, .context = Navigating },
        CommandKey { .name = "pgdn"sv, .does = "scroll a page down"sv, .context = Navigating },
        CommandKey { .name = "/"sv, .does = "filter"sv, .context = Navigating },
        CommandKey { .name = "?"sv, .does = "open the palette"sv, .context = Navigating },
        CommandKey { .name = "^k"sv, .does = "open the palette"sv, .context = Navigating },
        CommandKey { .name = "^p"sv, .does = "show the prompt"sv, .context = Navigating },
        CommandKey { .name = "b"sv, .does = "show the board"sv, .context = Navigating },
        CommandKey { .name = "t"sv, .does = "show the transcript"sv, .context = Navigating },
        CommandKey { .name = "d"sv, .does = "show the details"sv, .context = Navigating },
        CommandKey { .name = "e"sv, .does = "end the run"sv, .context = Navigating },
        CommandKey { .name = "o"sv, .does = "open the link"sv, .context = Navigating },
        CommandKey { .name = "r"sv, .does = "reveal the agent"sv, .context = Navigating },
        CommandKey { .name = "x"sv, .does = "check the worktree again"sv, .context = Navigating },
        CommandKey { .name = "z"sv, .does = "fold a group"sv, .context = Navigating },
        CommandKey { .name = "n"sv, .does = "start a new task"sv, .context = Navigating },
        CommandKey { .name = "w"sv, .does = "switch the view"sv, .context = Navigating },
        CommandKey { .name = "s"sv, .does = "sort"sv, .context = Navigating },
        CommandKey { .name = "m"sv, .does = "open the row menu"sv, .context = Navigating },
        CommandKey { .name = "1"sv, .does = "answer with quick reply 1"sv, .context = Navigating },
        CommandKey { .name = "2"sv, .does = "answer with quick reply 2"sv, .context = Navigating },
        CommandKey { .name = "3"sv, .does = "answer with quick reply 3"sv, .context = Navigating },
        CommandKey { .name = "4"sv, .does = "answer with quick reply 4"sv, .context = Navigating },
        CommandKey { .name = "5"sv, .does = "answer with quick reply 5"sv, .context = Navigating },
        CommandKey { .name = "6"sv, .does = "answer with quick reply 6"sv, .context = Navigating },
        CommandKey { .name = "7"sv, .does = "answer with quick reply 7"sv, .context = Navigating },
        CommandKey { .name = "8"sv, .does = "answer with quick reply 8"sv, .context = Navigating },
        CommandKey { .name = "9"sv, .does = "answer with quick reply 9"sv, .context = Navigating },
        CommandKey { .name = "enter"sv, .does = "send or start"sv, .context = Typing },
        CommandKey { .name = "tab"sv, .does = "move to the next field"sv, .context = Typing },
        CommandKey { .name = "^k"sv, .does = "open the palette"sv, .context = Typing },
        CommandKey { .name = "^u"sv, .does = "clear the text"sv, .context = Typing },
        CommandKey { .name = "^w"sv, .does = "drop a word"sv, .context = Typing },
        CommandKey { .name = "^p"sv,
                     .does = "show the prompt or choose the previous palette entry"sv,
                     .context = Typing },
        CommandKey { .name = "^n"sv, .does = "choose the next palette entry"sv, .context = Typing },
        CommandKey { .name = "up"sv, .does = "choose the previous palette entry"sv, .context = Typing },
        CommandKey { .name = "down"sv, .does = "choose the next palette entry"sv, .context = Typing },
    };

    /// OwnCommandKeys, then the message box's pass-through keys as Typing rows: one table each, no drift.
    constexpr auto CommandKeys = [] {
        auto keys = std::array<CommandKey, OwnCommandKeys.size() + MessagePassKeys.size()> {};
        auto const next = std::ranges::copy(OwnCommandKeys, keys.begin()).out;
        std::ranges::transform(MessagePassKeys, next, [](MessagePassKey const& pass) {
            return CommandKey { .name = pass.name, .does = pass.does, .context = Typing };
        });
        return keys;
    }();
} // namespace

std::span<DashboardKey const> dashboardKeys() noexcept
{
    return Keys;
}

std::span<CommandKey const> commandKeys() noexcept
{
    return CommandKeys;
}

std::span<MessagePassKey const> messagePassKeys() noexcept
{
    return MessagePassKeys;
}

} // namespace orchestrator
