// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator
{

/// What a navigation key does.
enum class NavAction : uint8_t
{
    MoveUp = 0,    ///< `move_up`: the previous row, setting or line.
    MoveDown,      ///< `move_down`: the next row, setting or line.
    First,         ///< `first`: the first row or setting, the top.
    Last,          ///< `last`: the last row or setting, the end.
    HalfPageUp,    ///< `half_page_up`: half a page up.
    HalfPageDown,  ///< `half_page_down`: half a page down.
    PreviousValue, ///< `previous_value`: the previous page, chip value or setting value.
    NextValue,     ///< `next_value`: the next page, chip value or setting value.
    LeaveField,    ///< `leave_field`: stop typing a text field.
    EnterField,    ///< `enter_field`: start typing a text field.
    Close,         ///< `close`: close what is open.
};

/// How many NavActions there are: one navActionRows() row each.
inline constexpr std::size_t NavActionCount = 11;

/// Where a navigation key acts: where nothing is typed, or in a text field being typed.
enum class NavContext : uint8_t
{
    Navigating = 0, ///< Nothing is being typed: letters may act.
    Typing,         ///< A text field is being typed: letters type.
};

/// Whether the vim keys are among the defaults (`keys.vim`).
enum class VimKeys : uint8_t
{
    Off = 0, ///< Only the plain keys (arrows, home, end, ...).
    On,      ///< The plain keys, then the vim keys.
};

/// One action: how `keys` names it, where it acts, and its default keys.
struct NavActionRow
{
    NavAction action;                            ///< The action; the row's index is its value.
    std::string_view configName;                 ///< Its key under `keys`, such as `move_down`.
    NavContext context;                          ///< Where its keys act.
    std::span<std::string_view const> plainKeys; ///< Its defaults whatever `keys.vim` says.
    std::span<std::string_view const> vimKeys;   ///< Its defaults while `keys.vim` is on.
};

/// @return One row per NavAction, in its order (a new action is a new row).
[[nodiscard]] std::span<NavActionRow const> navActionRows() noexcept;

/// The key names that are no character.
inline constexpr auto SpecialKeyNames = std::array {
    std::string_view { "up" },    std::string_view { "down" }, std::string_view { "left" },
    std::string_view { "right" }, std::string_view { "home" }, std::string_view { "end" },
    std::string_view { "pgup" },  std::string_view { "pgdn" }, std::string_view { "enter" },
    std::string_view { "esc" },   std::string_view { "tab" },  std::string_view { "del" },
};

/// What a key spelling is (Decision 7).
enum class KeyShape : uint8_t
{
    Invalid = 0, ///< No key spelling.
    Printable,   ///< One printable ASCII character other than space, such as `j` or `/`.
    Ctrl,        ///< `^` and a lower-case letter other than h, i, j or m, such as `^d`.
    Sequence,    ///< Two printable characters not starting with `^`, such as `gg`.
    Special,     ///< One of SpecialKeyNames.
};

/// @param name A key spelling, as `keys` writes it.
/// @return What it is; KeyShape::Invalid: no key.
[[nodiscard]] KeyShape keyShapeOf(std::string_view name) noexcept;

/// The navigation keys in force: each action's keys, and the action a key does per context.
class NavKeys
{
  public:
    /// @param vim Whether the vim keys are among them.
    /// @return Every row's plain keys, then its vim keys while @p vim is On.
    [[nodiscard]] static NavKeys defaults(VimKeys vim);

    /// Only navKeysOf() and defaults() build one: nothing else builds a NavKeys from raw lists.
    /// @param bindings Each action's keys, indexed by NavAction, already checked (navKeysOf()); a key listed
    ///        twice in one action is kept once.
    explicit NavKeys(std::array<std::vector<std::string>, NavActionCount> bindings);

    /// @param action An action.
    /// @return Its keys, in the configured order.
    [[nodiscard]] std::span<std::string const> keysOf(NavAction action) const noexcept;

    /// @param key A key spelling.
    /// @param context Where it was pressed.
    /// @return The action @p key does in @p context; std::nullopt: none.
    [[nodiscard]] std::optional<NavAction> actionFor(std::string_view key, NavContext context) const noexcept;

    /// @param key A key spelling.
    /// @param context Where it was pressed.
    /// @return Whether @p key is the first character of a sequence bound in @p context.
    [[nodiscard]] bool startsSequence(std::string_view key, NavContext context) const noexcept;

    [[nodiscard]] bool operator==(NavKeys const&) const = default;

  private:
    std::array<std::vector<std::string>, NavActionCount> _bindings;
};

/// Why a `keys` entry was refused.
enum class KeyRefusal : uint8_t
{
    NotAKey = 0, ///< No key spelling (keyShapeOf() is Invalid).
    TypesText,   ///< A key of a Typing action that would type: a character or a sequence.
    Taken,       ///< A key another action, or a command, already holds in the same context.
    Unbound,     ///< An action that must have a key (`close`, `leave_field`) was given none.
};

/// The configured entry refused, and why.
struct KeyBindingProblem
{
    NavAction action = NavAction::MoveUp;     ///< The action whose list holds the entry.
    std::optional<std::size_t> index = {};    ///< Into the action's configured list; none for Unbound.
    KeyRefusal refusal = KeyRefusal::NotAKey; ///< Why it was refused.
    std::string subject = {}; ///< For Taken: the key and its other holder (Decision 8 formats).
    [[nodiscard]] bool operator==(KeyBindingProblem const&) const = default;
};

/// @param vim Whether the vim keys are among the defaults.
/// @param configured Per action (NavAction order): its configured keys; std::nullopt keeps its defaults.
/// @return The keys in force, or the first entry refused (actions in row order, entries in list order).
[[nodiscard]] std::expected<NavKeys, KeyBindingProblem> navKeysOf(
    VimKeys vim, std::span<std::optional<std::vector<std::string>> const, NavActionCount> configured);

} // namespace orchestrator
