// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/NavKeys.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/DashboardKeys.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    constexpr auto NoKeys = std::array<std::string_view, 0> {};
    constexpr auto UpKeys = std::array { "up"sv };
    constexpr auto VimUpKeys = std::array { "k"sv };
    constexpr auto DownKeys = std::array { "down"sv };
    constexpr auto VimDownKeys = std::array { "j"sv };
    constexpr auto HomeKeys = std::array { "home"sv };
    constexpr auto VimFirstKeys = std::array { "gg"sv };
    constexpr auto EndKeys = std::array { "end"sv };
    constexpr auto VimLastKeys = std::array { "G"sv };
    constexpr auto VimHalfPageUpKeys = std::array { "^u"sv };
    constexpr auto VimHalfPageDownKeys = std::array { "^d"sv };
    constexpr auto LeftKeys = std::array { "left"sv };
    constexpr auto VimPreviousKeys = std::array { "h"sv };
    constexpr auto RightKeys = std::array { "right"sv };
    constexpr auto VimNextKeys = std::array { "l"sv };
    constexpr auto EscKeys = std::array { "esc"sv };
    constexpr auto EnterKeys = std::array { "enter"sv };
    constexpr auto VimEnterKeys = std::array { "i"sv };
    constexpr auto VimCloseKeys = std::array { "q"sv };

    constexpr auto Navigating = NavContext::Navigating;

    constexpr auto Rows = std::array {
        NavActionRow { .action = NavAction::MoveUp,
                       .configName = "move_up"sv,
                       .context = Navigating,
                       .plainKeys = UpKeys,
                       .vimKeys = VimUpKeys },
        NavActionRow { .action = NavAction::MoveDown,
                       .configName = "move_down"sv,
                       .context = Navigating,
                       .plainKeys = DownKeys,
                       .vimKeys = VimDownKeys },
        NavActionRow { .action = NavAction::First,
                       .configName = "first"sv,
                       .context = Navigating,
                       .plainKeys = HomeKeys,
                       .vimKeys = VimFirstKeys },
        NavActionRow { .action = NavAction::Last,
                       .configName = "last"sv,
                       .context = Navigating,
                       .plainKeys = EndKeys,
                       .vimKeys = VimLastKeys },
        NavActionRow { .action = NavAction::HalfPageUp,
                       .configName = "half_page_up"sv,
                       .context = Navigating,
                       .plainKeys = NoKeys,
                       .vimKeys = VimHalfPageUpKeys },
        NavActionRow { .action = NavAction::HalfPageDown,
                       .configName = "half_page_down"sv,
                       .context = Navigating,
                       .plainKeys = NoKeys,
                       .vimKeys = VimHalfPageDownKeys },
        NavActionRow { .action = NavAction::PreviousValue,
                       .configName = "previous_value"sv,
                       .context = Navigating,
                       .plainKeys = LeftKeys,
                       .vimKeys = VimPreviousKeys },
        NavActionRow { .action = NavAction::NextValue,
                       .configName = "next_value"sv,
                       .context = Navigating,
                       .plainKeys = RightKeys,
                       .vimKeys = VimNextKeys },
        NavActionRow { .action = NavAction::LeaveField,
                       .configName = "leave_field"sv,
                       .context = NavContext::Typing,
                       .plainKeys = EscKeys,
                       .vimKeys = NoKeys },
        NavActionRow { .action = NavAction::EnterField,
                       .configName = "enter_field"sv,
                       .context = Navigating,
                       .plainKeys = EnterKeys,
                       .vimKeys = VimEnterKeys },
        NavActionRow { .action = NavAction::Close,
                       .configName = "close"sv,
                       .context = Navigating,
                       .plainKeys = EscKeys,
                       .vimKeys = VimCloseKeys },
    };

    static_assert(Rows.size() == NavActionCount, "Rows has one row per NavAction");
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, Rows.size()),
                                      [](std::size_t i) {
                                          return static_cast<std::size_t>(Rows[i].action) == i;
                                      }),
                  "Rows is in NavAction order");

    /// The actions a dialog cannot do without: an empty list for one is refused (Unbound).
    constexpr auto RequiredActions = std::array { NavAction::LeaveField, NavAction::Close };

    using Bindings = std::array<std::vector<std::string>, NavActionCount>;

    [[nodiscard]] constexpr std::size_t indexOf(NavAction action) noexcept
    {
        return static_cast<std::size_t>(action);
    }

    [[nodiscard]] constexpr bool isPrintable(char character) noexcept
    {
        return character > ' ' && character <= '~';
    }

    /// @return Whether @p shape types into a text field.
    [[nodiscard]] constexpr bool typesText(KeyShape shape) noexcept
    {
        return shape == KeyShape::Printable || shape == KeyShape::Sequence;
    }

    /// @return Each list of @p bindings with every key once, in its first place.
    [[nodiscard]] Bindings uniqueEach(Bindings bindings)
    {
        for (auto& keys: bindings)
        {
            auto unique = std::vector<std::string> {};
            for (auto& key: keys)
                if (!std::ranges::contains(unique, key))
                    unique.push_back(std::move(key));
            keys = std::move(unique);
        }
        return bindings;
    }

    [[nodiscard]] Bindings defaultBindings(VimKeys vim)
    {
        auto bindings = Bindings {};
        for (auto const& row: Rows)
        {
            auto& keys = bindings[indexOf(row.action)];
            keys.assign(row.plainKeys.begin(), row.plainKeys.end());
            if (vim == VimKeys::On)
                keys.insert(keys.end(), row.vimKeys.begin(), row.vimKeys.end());
        }
        return bindings;
    }

    /// An action whose keys a configured entry may clash with.
    struct Holder
    {
        std::string_view configName;       ///< How `keys` names it.
        std::span<std::string const> keys; ///< Its keys in force.
    };

    /// @return The command @p name is in @p context, or nullptr.
    [[nodiscard]] CommandKey const* commandNamed(std::string_view name, NavContext context) noexcept
    {
        auto const command = std::ranges::find_if(
            commandKeys(), [&](CommandKey const& key) { return key.name == name && key.context == context; });
        return command != commandKeys().end() ? &*command : nullptr;
    }

    /// Checks one configured entry against the other holders of keys (Decision 8: a command, then an action,
    /// then a sequence prefix).
    /// @param key The entry, a valid spelling.
    /// @param row Its action.
    /// @param others The other actions of its context it may clash with: those keeping their defaults, and
    ///        the configured ones before it.
    /// @param own Its action's entries before it.
    /// @return The subject naming the other holder, or std::nullopt: no clash.
    [[nodiscard]] std::optional<std::string> clashOf(std::string_view key,
                                                     NavActionRow const& row,
                                                     std::span<Holder const> others,
                                                     Holder const& own)
    {
        // A binding may take a key in its own action's default row (enter_field's enter, close's esc).
        auto const ownDefault =
            std::ranges::contains(row.plainKeys, key) || std::ranges::contains(row.vimKeys, key);
        auto const command = [&](std::string_view name) {
            return ownDefault ? nullptr : commandNamed(name, row.context);
        };
        if (auto const* const holder = command(key))
            return std::format("{}, the dashboard's key to {}", key, holder->does);
        for (auto const& holder: others)
            if (std::ranges::contains(holder.keys, key))
                return std::format("{}, also keys.{}", key, holder.configName);

        auto holders = std::vector<Holder>(others.begin(), others.end());
        holders.push_back(own);
        auto const shape = keyShapeOf(key);
        if (shape == KeyShape::Sequence)
        {
            auto const prefix = key.substr(0, 1);
            if (auto const* const holder = command(prefix))
                return std::format("{}, which starts with the dashboard's key to {}", key, holder->does);
            for (auto const& holder: holders)
                if (std::ranges::contains(holder.keys, prefix))
                    return std::format("{}, which starts with keys.{}'s {}", key, holder.configName, prefix);
        }
        if (shape == KeyShape::Printable)
            for (auto const& holder: holders)
                for (auto const& sequence: holder.keys)
                    if (keyShapeOf(sequence) == KeyShape::Sequence && sequence.front() == key.front())
                        return std::format("{}, which starts keys.{}'s {}", key, holder.configName, sequence);
        return std::nullopt;
    }

} // namespace

std::span<NavActionRow const> navActionRows() noexcept
{
    return Rows;
}

KeyShape keyShapeOf(std::string_view name) noexcept
{
    if (std::ranges::contains(SpecialKeyNames, name))
        return KeyShape::Special;
    if (name.size() == 1)
        return isPrintable(name.front()) ? KeyShape::Printable : KeyShape::Invalid;
    if (name.size() != 2 || !isPrintable(name.front()) || !isPrintable(name.back()))
        return KeyShape::Invalid;
    if (name.front() != '^')
        return KeyShape::Sequence;
    // Terminals send ^h, ^i, ^j and ^m as Backspace, Tab, LF and CR.
    auto const letter = name.back();
    return letter >= 'a' && letter <= 'z' && !"hijm"sv.contains(letter) ? KeyShape::Ctrl : KeyShape::Invalid;
}

NavKeys NavKeys::defaults(VimKeys vim)
{
    return NavKeys { defaultBindings(vim) };
}

NavKeys::NavKeys(std::array<std::vector<std::string>, NavActionCount> bindings):
    _bindings { uniqueEach(std::move(bindings)) }
{
}

std::span<std::string const> NavKeys::keysOf(NavAction action) const noexcept
{
    auto const index = indexOf(action);
    return index < _bindings.size() ? std::span<std::string const> { _bindings[index] }
                                    : std::span<std::string const> {};
}

std::optional<NavAction> NavKeys::actionFor(std::string_view key, NavContext context) const noexcept
{
    for (auto const& row: Rows)
        if (row.context == context && std::ranges::contains(_bindings[indexOf(row.action)], key))
            return row.action;
    return std::nullopt;
}

bool NavKeys::startsSequence(std::string_view key, NavContext context) const noexcept
{
    if (key.size() != 1)
        return false;
    return std::ranges::any_of(Rows, [&](NavActionRow const& row) {
        return row.context == context
               && std::ranges::any_of(_bindings[indexOf(row.action)], [&](std::string const& bound) {
                      return keyShapeOf(bound) == KeyShape::Sequence && bound.front() == key.front();
                  });
    });
}

std::expected<NavKeys, KeyBindingProblem> navKeysOf(
    VimKeys vim, std::span<std::optional<std::vector<std::string>> const, NavActionCount> configured)
{
    auto inForce = defaultBindings(vim);
    for (auto const& row: Rows)
        if (auto const& list = configured[indexOf(row.action)])
            inForce[indexOf(row.action)] = *list;

    for (auto const& row: Rows)
    {
        auto const& list = configured[indexOf(row.action)];
        if (!list)
            continue;
        if (list->empty() && std::ranges::contains(RequiredActions, row.action))
            return std::unexpected(
                KeyBindingProblem { .action = row.action, .refusal = KeyRefusal::Unbound });
        // When both holders are configured, the later one in row order is refused.
        auto others = std::vector<Holder> {};
        for (auto const& other: Rows)
            if (other.action != row.action && other.context == row.context
                && (!configured[indexOf(other.action)] || other.action < row.action))
                others.push_back(
                    Holder { .configName = other.configName, .keys = inForce[indexOf(other.action)] });
        for (auto const [index, key]: std::views::enumerate(*list))
        {
            auto const at = static_cast<std::size_t>(index);
            auto const refuse = [&](KeyRefusal refusal, std::string subject = {}) {
                return std::unexpected(KeyBindingProblem {
                    .action = row.action, .index = at, .refusal = refusal, .subject = std::move(subject) });
            };
            auto const shape = keyShapeOf(key);
            if (shape == KeyShape::Invalid)
                return refuse(KeyRefusal::NotAKey);
            if (row.context == NavContext::Typing && typesText(shape))
                return refuse(KeyRefusal::TypesText);
            auto const own = Holder { .configName = row.configName,
                                      .keys = std::span<std::string const> { *list }.first(at) };
            if (auto subject = clashOf(key, row, others, own))
                return refuse(KeyRefusal::Taken, std::move(*subject));
        }
    }
    return NavKeys { std::move(inForce) };
}

} // namespace orchestrator
