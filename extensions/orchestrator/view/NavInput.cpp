// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/NavInput.hpp>

#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Utf8.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    /// The name of a key that is not a character.
    struct SpecialKey
    {
        core::tui::KeyCode code = core::tui::KeyCode::None; ///< The key.
        std::string_view name = {};                         ///< Its spelling (SpecialKeyNames).
        std::string_view shown = {};                        ///< How the bar and the palette name it.
    };

    constexpr auto SpecialKeys = std::array {
        SpecialKey { .code = core::tui::KeyCode::Up, .name = "up"sv, .shown = "↑"sv },
        SpecialKey { .code = core::tui::KeyCode::Down, .name = "down"sv, .shown = "↓"sv },
        SpecialKey { .code = core::tui::KeyCode::Home, .name = "home"sv, .shown = "home"sv },
        SpecialKey { .code = core::tui::KeyCode::End, .name = "end"sv, .shown = "end"sv },
        SpecialKey { .code = core::tui::KeyCode::Enter, .name = "enter"sv, .shown = "⏎"sv },
        SpecialKey { .code = core::tui::KeyCode::Escape, .name = "esc"sv, .shown = "esc"sv },
        SpecialKey { .code = core::tui::KeyCode::Tab, .name = "tab"sv, .shown = "tab"sv },
        SpecialKey { .code = core::tui::KeyCode::Delete, .name = "del"sv, .shown = "del"sv },
        SpecialKey { .code = core::tui::KeyCode::PageUp, .name = "pgup"sv, .shown = "pgup"sv },
        SpecialKey { .code = core::tui::KeyCode::PageDown, .name = "pgdn"sv, .shown = "pgdn"sv },
        SpecialKey { .code = core::tui::KeyCode::Left, .name = "left"sv, .shown = "←"sv },
        SpecialKey { .code = core::tui::KeyCode::Right, .name = "right"sv, .shown = "→"sv },
    };

    static_assert(SpecialKeys.size() == SpecialKeyNames.size(), "every special key name has a key");

    /// The Ctrl chords a binding names, as `^` and the letter: every letter but h, i, j and m, which
    /// terminals send as Backspace, Tab, LF and CR.
    constexpr auto CtrlLetters = [] {
        auto letters = std::array<char, 26 - 4> {};
        auto at = std::size_t { 0 };
        for (auto const letter: std::views::iota('a', static_cast<char>('z' + 1)))
            if (!"hijm"sv.contains(letter))
                letters[at++] = letter;
        return letters;
    }();

    /// The command each movement action is in each view's list (Decision 9).
    struct ListCommandRow
    {
        NavAction action = NavAction::MoveUp; ///< The action.
        KeyCommand agents = KeyCommand::Up;   ///< Its command in the Fleet and the Focus.
        KeyCommand work = KeyCommand::Up;     ///< Its command in the Work list.
    };

    constexpr auto ListCommands = std::array {
        ListCommandRow { .action = NavAction::MoveUp, .agents = KeyCommand::Up, .work = KeyCommand::Up },
        ListCommandRow {
            .action = NavAction::MoveDown, .agents = KeyCommand::Down, .work = KeyCommand::Down },
        ListCommandRow { .action = NavAction::First, .agents = KeyCommand::Home, .work = KeyCommand::Home },
        ListCommandRow { .action = NavAction::Last, .agents = KeyCommand::End, .work = KeyCommand::End },
        ListCommandRow { .action = NavAction::HalfPageUp,
                         .agents = KeyCommand::HalfPageUp,
                         .work = KeyCommand::HalfPageUp },
        ListCommandRow { .action = NavAction::HalfPageDown,
                         .agents = KeyCommand::HalfPageDown,
                         .work = KeyCommand::HalfPageDown },
        ListCommandRow { .action = NavAction::PreviousValue,
                         .agents = KeyCommand::PreviousPage,
                         .work = KeyCommand::RepoPrevious },
        ListCommandRow {
            .action = NavAction::NextValue, .agents = KeyCommand::NextPage, .work = KeyCommand::RepoNext },
    };

    /// @return The defaults, with the vim keys: the keys in force without a configuration.
    [[nodiscard]] NavKeys const& defaultKeys() noexcept
    {
        static auto const keys = NavKeys::defaults(VimKeys::On);
        return keys;
    }
} // namespace

std::string keyNameFor(core::tui::KeyEvent const& key)
{
    if (core::tui::hasModifier(key.modifiers, core::tui::Modifier::Alt))
        return {};
    if (core::tui::hasModifier(key.modifiers, core::tui::Modifier::Ctrl))
    {
        auto const letter = std::ranges::find_if(
            CtrlLetters, [&](char candidate) { return isCtrl(key, static_cast<char32_t>(candidate)); });
        return letter != CtrlLetters.end() ? std::format("^{}", *letter) : std::string {};
    }
    if (auto const special = std::ranges::find(SpecialKeys, key.key, &SpecialKey::code);
        special != SpecialKeys.end())
        return std::string { special->name };
    auto name = std::string {};
    if (auto const printable = printableOf(key))
        appendUtf8(name, *printable);
    return name;
}

std::string_view shownKeyName(std::string_view name) noexcept
{
    auto const special = std::ranges::find(SpecialKeys, name, &SpecialKey::name);
    return special != SpecialKeys.end() ? special->shown : name;
}

NavKeys const& keysInForce(Engine const& engine) noexcept
{
    auto const* const config = engine.config();
    return config != nullptr ? config->keys() : defaultKeys();
}

NavKeys const& keysInForce(Engine const* engine) noexcept
{
    return engine != nullptr ? keysInForce(*engine) : defaultKeys();
}

std::expected<NavAction, NavMiss> navActionOf(core::tui::KeyEvent const& key,
                                              NavContext context,
                                              NavKeys const& keys,
                                              std::string& pending)
{
    auto const name = keyNameFor(key);
    // A pending first key completes its sequence with this one, or is dropped: this key then acts alone.
    if (auto const first = std::exchange(pending, {}); !first.empty() && !name.empty())
        if (auto const action = keys.actionFor(first + name, context))
            return *action;
    if (name.empty())
        return std::unexpected(NavMiss::NotBound);
    if (keys.startsSequence(name, context))
    {
        pending = name;
        return std::unexpected(NavMiss::Pending);
    }
    if (auto const action = keys.actionFor(name, context))
        return *action;
    return std::unexpected(NavMiss::NotBound);
}

std::optional<NavAction> passedKeyAction(core::tui::KeyEvent const& key, NavKeys const& keys)
{
    using core::tui::Modifier;
    if (core::tui::hasModifier(key.modifiers, Modifier::Ctrl)
        || core::tui::hasModifier(key.modifiers, Modifier::Alt)
        || keyShapeOf(keyNameFor(key)) != KeyShape::Special)
        return std::nullopt;
    auto none = std::string {};
    auto const action = navActionOf(key, NavContext::Navigating, keys, none);
    return action ? std::optional { *action } : std::nullopt;
}

NavContext navContextOf(Interaction const& interaction, KeyContext const& context) noexcept
{
    auto const contextOf = [](FieldMode mode) {
        return mode == FieldMode::Typing ? NavContext::Typing : NavContext::Navigating;
    };
    if (interaction.startDialog)
        return contextOf(interaction.startDialog->mode);
    if (interaction.taskBox)
        return contextOf(interaction.taskBox->mode);
    if (interaction.view == View::Work)
        return interaction.work.filtering == Filtering::Typing ? NavContext::Typing : NavContext::Navigating;
    auto const inMessageBox =
        interaction.place == Place::Focus && hasMessageBox(context.engine, context.selected);
    return interaction.filtering == Filtering::Typing || inMessageBox ? NavContext::Typing
                                                                      : NavContext::Navigating;
}

std::optional<KeyCommand> listCommandOf(NavAction action, View view) noexcept
{
    auto const row = std::ranges::find(ListCommands, action, &ListCommandRow::action);
    if (row == ListCommands.end())
        return std::nullopt;
    return view == View::Work ? row->work : row->agents;
}

std::string navKeyLabel(NavKeys const& keys, NavAction action)
{
    auto const bound = keys.keysOf(action);
    // `⏎` is every surface's own key: the hint for entering a field names the other one.
    auto const first =
        action == NavAction::EnterField
            ? std::ranges::find_if(bound, [](std::string const& key) { return key != "enter"; })
            : bound.begin();
    return first != bound.end() ? std::string { shownKeyName(*first) } : std::string {};
}

} // namespace orchestrator::view
