// SPDX-License-Identifier: Apache-2.0
//
// The default bindings must be unambiguous: no key press may be claimed by two of them, because
// config::apply() runs only the first match and the second would be dead.

#include <contour/config/Actions.hpp>
#include <contour/config/BindingCollisions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DefaultBindings.hpp>
#include <contour/config/ModifierNames.hpp>
#include <contour/config/PortableSpelling.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace contour;
using namespace contour::config;
using vtbackend::MatchModes;
using vtbackend::Modifier;
using vtbackend::Modifiers;

namespace
{
/// Every keyboard platform kind.
constexpr auto const& AllKinds = AllKeyboardPlatformKinds;

MatchModes modesOf(std::initializer_list<MatchModes::Flag> on,
                   std::initializer_list<MatchModes::Flag> off = {})
{
    auto modes = MatchModes {};
    for (auto const flag: on)
        modes.enable(flag);
    for (auto const flag: off)
        modes.disable(flag);
    return modes;
}

CharInputMapping ctrlC(MatchModes modes, actions::Action action)
{
    return CharInputMapping {
        .modes = modes, .modifiers = Modifiers { Modifier::Control }, .input = U'C', .binding = { action }
    };
}

std::string describe(InputMappings const& mappings)
{
    auto text = std::string {};
    for (auto const& collision: findBindingCollisions(mappings))
        text += std::format("[kind {} #{} / #{}: {}] ",
                            static_cast<int>(collision.kind),
                            collision.first,
                            collision.second,
                            collision.chord);
    return text;
}

/// Action names, in table order.
using Names = std::vector<std::string_view>;

/// Appends to @p out the action of every row of @p rows on one chord, skipping repeats.
/// @param modes When not empty, only rows requiring exactly these modes.
template <typename Rows, typename Input>
void appendActions(Rows const& rows, Modifiers modifiers, Input input, MatchModes modes, Names& out)
{
    for (auto const& row: rows)
    {
        if (row.modifiers != modifiers || row.input != input || row.binding.empty())
            continue;
        if (modes != MatchModes {} && row.modes != modes)
            continue;
        auto const name = actions::name(row.binding.front());
        if (std::ranges::find(out, name) == out.end())
            out.push_back(name);
    }
}

/// The fallback rows of @p fallbacks as plain bindings.
template <typename Input>
[[nodiscard]] auto mappingsOf(std::vector<FallbackMapping<Input>> const& fallbacks)
{
    return fallbacks | std::views::transform(&FallbackMapping<Input>::mapping);
}

/// The actions bound to a character chord on @p kind, in either edition's defaults or the fallbacks.
/// @param modes When not empty, only rows requiring exactly these modes.
[[nodiscard]] Names charActions(KeyboardPlatformKind kind,
                                Modifiers modifiers,
                                char32_t input,
                                MatchModes modes = {})
{
    auto result = Names {};
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        appendActions(defaultInputMappings(edition, kind).charMappings, modifiers, input, modes, result);
    appendActions(mappingsOf(builtinFallbackCharMappings(kind)), modifiers, input, modes, result);
    return result;
}

/// The actions bound to a key chord on @p kind, in either edition's defaults or the fallbacks.
/// @param modes When not empty, only rows requiring exactly these modes.
[[nodiscard]] Names keyActions(KeyboardPlatformKind kind,
                               Modifiers modifiers,
                               vtbackend::Key input,
                               MatchModes modes = {})
{
    auto result = Names {};
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        appendActions(defaultInputMappings(edition, kind).keyMappings, modifiers, input, modes, result);
    appendActions(mappingsOf(builtinFallbackKeyMappings(kind)), modifiers, input, modes, result);
    return result;
}

/// The actions bound to a character chord on @p kind in the fallback table only.
[[nodiscard]] Names fallbackCharActions(KeyboardPlatformKind kind, Modifiers modifiers, char32_t input)
{
    auto result = Names {};
    appendActions(mappingsOf(builtinFallbackCharMappings(kind)), modifiers, input, MatchModes {}, result);
    return result;
}

/// The first character fallback row on a chord on @p kind, or nullptr.
[[nodiscard]] FallbackCharMapping const* fallbackCharRow(KeyboardPlatformKind kind,
                                                         Modifiers modifiers,
                                                         char32_t input)
{
    auto const& rows = builtinFallbackCharMappings(kind);
    auto const found = std::ranges::find_if(rows, [&](auto const& row) {
        return row.mapping.modifiers == modifiers && row.mapping.input == input;
    });
    return found != rows.end() ? &*found : nullptr;
}

/// @p rows as a configuration written on @p from reads them back on @p to (@see portablePrimarySpellings).
template <typename Input>
[[nodiscard]] std::vector<vtbackend::InputBinding<Input, ActionList>> carried(
    std::vector<vtbackend::InputBinding<Input, ActionList>> rows,
    KeyboardPlatformKind from,
    KeyboardPlatformKind to)
{
    auto const spellings =
        portablePrimarySpellings(std::span<vtbackend::InputBinding<Input, ActionList> const> { rows }, from);
    for (auto&& [row, spelling]: std::views::zip(rows, spellings))
        row.modifiers = modifiersReadOn(row.modifiers, from, spelling, to);
    return rows;
}

/// A configuration's bindings, written on @p from and read back on @p to.
[[nodiscard]] InputMappings carriedMappings(InputMappings const& mappings,
                                            KeyboardPlatformKind from,
                                            KeyboardPlatformKind to)
{
    return InputMappings { .keyMappings = carried(mappings.keyMappings, from, to),
                           .charMappings = carried(mappings.charMappings, from, to),
                           .mouseMappings = carried(mappings.mouseMappings, from, to) };
}
} // namespace

TEST_CASE("modesOverlap", "[bindings]")
{
    CHECK(modesOverlap(MatchModes {}, modesOf({ MatchModes::Insert })));
    CHECK_FALSE(modesOverlap(modesOf({ MatchModes::Search }), modesOf({}, { MatchModes::Search })));
    CHECK(modesOverlap(modesOf({ MatchModes::Select, MatchModes::Insert }), modesOf({ MatchModes::Select })));
}

TEST_CASE("findBindingCollisions finds a crafted pair, and only that", "[bindings]")
{
    SECTION("equal modes collide")
    {
        auto mappings = InputMappings {};
        mappings.charMappings.push_back(ctrlC(MatchModes {}, actions::CopySelection {}));
        mappings.charMappings.push_back(ctrlC(MatchModes {}, actions::CancelSelection {}));
        auto const found = findBindingCollisions(mappings);
        REQUIRE(found.size() == 1);
        CHECK(found[0].kind == BindingKind::Char);
        CHECK(found[0].first == 0);
        CHECK(found[0].second == 1);
        CHECK_FALSE(found[0].chord.empty());
    }

    SECTION("disjoint modes do not")
    {
        auto mappings = InputMappings {};
        mappings.charMappings.push_back(ctrlC(modesOf({ MatchModes::Search }), actions::CopySelection {}));
        mappings.charMappings.push_back(
            ctrlC(modesOf({}, { MatchModes::Search }), actions::CancelSelection {}));
        CHECK(findBindingCollisions(mappings).empty());
    }

    SECTION("mouse bindings differing in click count do not")
    {
        auto mappings = InputMappings {};
        for (auto const clicks: { uint8_t { 2 }, uint8_t { 3 } })
            mappings.mouseMappings.push_back(MouseInputMapping { .modes = MatchModes {},
                                                                 .modifiers = Modifiers {},
                                                                 .input = vtbackend::MouseButton::Left,
                                                                 .binding = { actions::CopySelection {} },
                                                                 .clickCount = clicks });
        CHECK(findBindingCollisions(mappings).empty());
    }
}

TEST_CASE("no default chord fires twice", "[bindings]")
{
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        for (auto const kind: AllKinds)
        {
            INFO("edition " << token << ", platform " << std::to_underlying(kind));
            auto const& mappings = defaultInputMappings(edition, kind);
            CHECK(findBindingCollisions(mappings).empty());
            if (!findBindingCollisions(mappings).empty())
                FAIL_CHECK(describe(mappings));
        }
}

TEST_CASE("the daemon and workspace switching actions are bound by no default chord", "[bindings][switch]")
{
    // Deliberately unbound, CycleWorkspace aside (its own case below): every plausible chord is a shell,
    // editor or tmux key somewhere, and the palette, the title bar and the switcher reach them. A user binds
    // the ones they want.
    using namespace contour::actions;
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        for (auto const kind: AllKinds)
        {
            INFO("edition " << token << ", platform " << std::to_underlying(kind));
            auto const& mappings = defaultInputMappings(edition, kind);
            auto const switching = [](auto const& mapping) {
                return std::ranges::any_of(mapping.binding, [](auto const& action) {
                    return std::holds_alternative<SwitchToWorkspace>(action)
                           || std::holds_alternative<OpenWorkspace>(action)
                           || std::holds_alternative<CycleDaemon>(action)
                           || std::holds_alternative<SwitchToDaemon>(action)
                           || std::holds_alternative<OpenDaemon>(action)
                           || std::holds_alternative<ShowSwitcher>(action)
                           || std::holds_alternative<NewDaemon>(action)
                           || std::holds_alternative<UpgradeDaemon>(action);
                });
            };
            CHECK(std::ranges::none_of(mappings.keyMappings, switching));
            CHECK(std::ranges::none_of(mappings.charMappings, switching));
            CHECK(std::ranges::none_of(mappings.mouseMappings, switching));
        }
}

TEST_CASE("every edition and platform cycles workspaces one level up from the tab chords",
          "[bindings][switch]")
{
    // Ctrl+PageUp/PageDown cycle the tabs; Shift and Alt added cycle the workspaces (Ctrl+Alt alone is
    // Emacs's C-M-prior/next). macOS takes Control+Command with the vertical arrows. In the written defaults
    // and the fallbacks alike, so a configuration that predates them still has them.
    using actions::CycleDirection;
    using actions::CycleWorkspace;
    using vtbackend::Key;
    auto const cycles = [](auto const& mappings, Modifiers mods, Key key, CycleDirection direction) {
        return std::ranges::any_of(mappings, [&](auto const& mapping) {
            return mapping.modifiers == mods && mapping.input == key && mapping.binding.size() == 1
                   && std::holds_alternative<CycleWorkspace>(mapping.binding.front())
                   && std::get<CycleWorkspace>(mapping.binding.front()).direction == direction;
        });
    };
    for (auto const kind: AllKinds)
    {
        auto const mac = kind == KeyboardPlatformKind::Mac;
        auto const mods = mac ? Modifiers { Modifier::Control, Modifier::Super }
                              : Modifiers { Modifier::Control, Modifier::Shift, Modifier::Alt };
        auto const nextKey = mac ? Key::DownArrow : Key::PageDown;
        auto const previousKey = mac ? Key::UpArrow : Key::PageUp;
        INFO("platform " << std::to_underlying(kind));
        for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        {
            INFO("edition " << token);
            auto const& mappings = defaultInputMappings(edition, kind).keyMappings;
            CHECK(cycles(mappings, mods, nextKey, CycleDirection::Next));
            CHECK(cycles(mappings, mods, previousKey, CycleDirection::Previous));
        }
        auto const fallbacks = mappingsOf(builtinFallbackKeyMappings(kind));
        CHECK(cycles(fallbacks, mods, nextKey, CycleDirection::Next));
        CHECK(cycles(fallbacks, mods, previousKey, CycleDirection::Previous));
    }
}

TEST_CASE("a bare F6 is bound in no edition", "[bindings]")
{
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
    {
        auto const& mappings = defaultInputMappings(edition);
        CHECK(std::ranges::none_of(mappings.keyMappings, [](auto const& mapping) {
            return mapping.input == vtbackend::Key::F6 && mapping.modifiers == Modifiers {};
        }));
    }
}

TEST_CASE("copying with Ctrl+C still clears the selection", "[bindings]")
{
    auto const& mappings = defaultInputMappings(DefaultsEdition::Native2026);
    auto const modes = modesOf({ MatchModes::Select, MatchModes::Insert });
    auto const found = std::ranges::find_if(mappings.charMappings, [&](auto const& mapping) {
        return mapping.input == U'C' && mapping.modifiers == Modifiers { Modifier::Control }
               && mapping.modes == modes;
    });
    REQUIRE(found != mappings.charMappings.end());
    REQUIRE(found->binding.size() == 2);
    CHECK(std::holds_alternative<actions::CopySelection>(found->binding[0]));
    CHECK(std::holds_alternative<actions::CancelSelection>(found->binding[1]));
}

namespace
{
template <typename Action>
bool binds(InputMappings const& mappings, vtbackend::Key key, Modifiers modifiers)
{
    return std::ranges::any_of(mappings.keyMappings, [&](auto const& mapping) {
        return mapping.input == key && mapping.modifiers == modifiers && mapping.modes == MatchModes {}
               && mapping.binding.size() == 1 && std::holds_alternative<Action>(mapping.binding.front());
    });
}
} // namespace

TEST_CASE("Native2026 binds the chrome focus chords and Classic none of them", "[bindings]")
{
    auto const& native = defaultInputMappings(DefaultsEdition::Native2026);
    CHECK(binds<actions::FocusNextArea>(native, vtbackend::Key::F6, Modifiers { Modifier::Control }));
    CHECK(binds<actions::FocusPreviousArea>(
        native, vtbackend::Key::F6, Modifiers { Modifier::Control, Modifier::Shift }));
    // The toolbar chord is macOS's alone.
    CHECK(binds<actions::FocusToolbar>(
        defaultInputMappings(DefaultsEdition::Native2026, KeyboardPlatformKind::Mac),
        vtbackend::Key::F5,
        Modifiers { Modifier::Control }));
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 })
        CHECK(std::ranges::none_of(
            defaultInputMappings(DefaultsEdition::Native2026, kind).keyMappings, [](auto const& mapping) {
                return std::holds_alternative<actions::FocusToolbar>(mapping.binding.front());
            }));

    auto const& classic = defaultInputMappings(DefaultsEdition::Classic);
    CHECK(std::ranges::none_of(classic.keyMappings, [](auto const& mapping) {
        auto const& action = mapping.binding.front();
        return std::holds_alternative<actions::FocusNextArea>(action)
               || std::holds_alternative<actions::FocusPreviousArea>(action)
               || std::holds_alternative<actions::FocusToolbar>(action);
    }));
}

namespace
{
/// The pane-managing actions: what moves between panes, splits one, zooms one, or moves the active tab.
bool isPaneAction(actions::Action const& action)
{
    return std::holds_alternative<actions::FocusPaneLeft>(action)
           || std::holds_alternative<actions::FocusPaneRight>(action)
           || std::holds_alternative<actions::FocusPaneUp>(action)
           || std::holds_alternative<actions::FocusPaneDown>(action)
           || std::holds_alternative<actions::SplitVertical>(action)
           || std::holds_alternative<actions::SplitHorizontal>(action)
           || std::holds_alternative<actions::MoveTabToLeft>(action)
           || std::holds_alternative<actions::MoveTabToRight>(action)
           || std::holds_alternative<actions::NewWindowTo>(action);
}

/// The pane rows of a fallback table.
template <typename Mapping>
std::vector<Mapping> paneRows(std::vector<Mapping> const& table)
{
    auto result = std::vector<Mapping> {};
    std::ranges::copy_if(table, std::back_inserter(result), [](auto const& row) {
        return row.mapping.binding.size() == 1 && isPaneAction(row.mapping.binding.front());
    });
    return result;
}

/// Whether @p mappings binds exactly @p modifiers and @p input to a pane action.
template <typename Mapping, typename Input>
bool bindsPaneActionAt(std::vector<Mapping> const& mappings, Modifiers modifiers, Input input)
{
    return std::ranges::count_if(
               mappings, [&](auto const& have) { return have.input == input && have.modifiers == modifiers; })
           == 1;
}
} // namespace

TEST_CASE("every platform can focus all four neighbours and split both ways", "[bindings][panes]")
{
    for (auto const kind: AllKinds)
    {
        INFO("platform " << std::to_underlying(kind));
        auto const has = [&](auto const& table, auto const& wanted) {
            return std::ranges::any_of(table, [&](auto const& row) {
                return row.mapping.binding.size() == 1
                       && std::holds_alternative<std::remove_cvref_t<decltype(wanted)>>(
                           row.mapping.binding.front());
            });
        };
        auto const& keys = builtinFallbackKeyMappings(kind);
        auto const& chars = builtinFallbackCharMappings(kind);
        CHECK(has(keys, actions::FocusPaneLeft {}));
        CHECK(has(keys, actions::FocusPaneRight {}));
        CHECK(has(keys, actions::FocusPaneUp {}));
        CHECK(has(keys, actions::FocusPaneDown {}));
        CHECK(has(chars, actions::SplitVertical {}));
        CHECK(has(chars, actions::SplitHorizontal {}));
    }
}

TEST_CASE("every platform moves the active tab with the browsers' chords", "[bindings][panes]")
{
    struct Row
    {
        KeyboardPlatformKind kind;
        Modifiers modifiers;
    };
    auto const rows = std::array {
        Row { .kind = KeyboardPlatformKind::Xkb,
              .modifiers = Modifiers { Modifier::Control, Modifier::Shift } },
        Row { .kind = KeyboardPlatformKind::Win32,
              .modifiers = Modifiers { Modifier::Control, Modifier::Shift } },
        Row { .kind = KeyboardPlatformKind::Mac, .modifiers = Modifiers { Modifier::Super } },
    };
    for (auto const& [kind, modifiers]: rows)
    {
        INFO("platform " << std::to_underlying(kind));
        auto const actionOf = [&](vtbackend::Key key) {
            auto const& keys = builtinFallbackKeyMappings(kind);
            auto const found = std::ranges::find_if(keys, [&](auto const& row) {
                return row.mapping.input == key && row.mapping.modifiers == modifiers
                       && isPaneAction(row.mapping.binding.front());
            });
            REQUIRE(found != keys.end());
            return found->mapping.binding.front();
        };
        CHECK(std::holds_alternative<actions::MoveTabToLeft>(actionOf(vtbackend::Key::PageUp)));
        CHECK(std::holds_alternative<actions::MoveTabToRight>(actionOf(vtbackend::Key::PageDown)));
    }
}

TEST_CASE("the macOS pane chords are the Command chords", "[bindings][panes]")
{
    auto const superOnly = Modifiers { Modifier::Super };
    auto const superShift = Modifiers { Modifier::Super, Modifier::Shift };
    auto const split = [&](Modifiers modifiers) {
        auto const& chars = builtinFallbackCharMappings(KeyboardPlatformKind::Mac);
        auto const found = std::ranges::find_if(chars, [&](auto const& row) {
            return row.mapping.input == U'D' && row.mapping.modifiers == modifiers;
        });
        REQUIRE(found != chars.end());
        return found->mapping.binding.front();
    };
    CHECK(std::holds_alternative<actions::SplitVertical>(split(superOnly)));
    CHECK(std::holds_alternative<actions::SplitHorizontal>(split(superShift)));
}

TEST_CASE("a platform's pane chords are in its defaults and in its fallbacks", "[bindings][panes]")
{
    for (auto const kind: AllKinds)
        for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        {
            INFO("platform " << std::to_underlying(kind) << ", edition " << token);
            auto const& defaults = defaultInputMappings(edition, kind);
            auto const keyRows = paneRows(builtinFallbackKeyMappings(kind));
            auto const charRows = paneRows(builtinFallbackCharMappings(kind));
            CHECK(!keyRows.empty());
            CHECK(!charRows.empty());
            for (auto const& row: keyRows)
                CHECK(bindsPaneActionAt(defaults.keyMappings, row.mapping.modifiers, row.mapping.input));
            for (auto const& row: charRows)
                CHECK(bindsPaneActionAt(defaults.charMappings, row.mapping.modifiers, row.mapping.input));
        }
}

TEST_CASE("every platform opens the Connect picker with its New Window to… chord", "[bindings][connect]")
{
    struct Row
    {
        KeyboardPlatformKind kind;
        Modifiers modifiers;
    };
    auto const rows = std::array {
        Row { .kind = KeyboardPlatformKind::Xkb,
              .modifiers = Modifiers { Modifier::Control, Modifier::Shift } },
        Row { .kind = KeyboardPlatformKind::Win32,
              .modifiers = Modifiers { Modifier::Control, Modifier::Shift } },
        Row { .kind = KeyboardPlatformKind::Mac,
              .modifiers = Modifiers { Modifier::Super, Modifier::Shift } },
    };
    for (auto const& [kind, modifiers]: rows)
    {
        INFO("platform " << std::to_underlying(kind));
        auto const& chars = builtinFallbackCharMappings(kind);
        auto const found = std::ranges::find_if(chars, [&](auto const& row) {
            return row.mapping.input == U'K' && row.mapping.modifiers == modifiers;
        });
        REQUIRE(found != chars.end());
        CHECK(std::holds_alternative<actions::NewWindowTo>(found->mapping.binding.front()));
        // No edition's defaults bind the chord to anything else.
        for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        {
            auto const& mappings = defaultInputMappings(edition, kind);
            auto const claimed = std::ranges::count_if(mappings.charMappings, [&](auto const& have) {
                return have.input == U'K' && have.modifiers == modifiers;
            });
            CHECK(claimed == 1);
        }
    }
}

TEST_CASE("macOS opens the command palette with Command+P and Shift+Command+P", "[bindings][palette]")
{
    auto const mac = KeyboardPlatformKind::Mac;
    CHECK(charActions(mac, Modifiers { Modifier::Super }, U'P') == Names { "OpenCommandPalette" });
    CHECK(charActions(mac, Modifiers { Modifier::Super, Modifier::Shift }, U'P')
          == Names { "OpenCommandPalette" });
    // A configuration written before Command+P existed lists Shift+Command+P only; Command+P reaches it as a
    // fallback.
    CHECK(fallbackCharActions(mac, Modifiers { Modifier::Super }, U'P') == Names { "OpenCommandPalette" });
}

TEST_CASE("every platform closes the active pane with Primary+W", "[bindings][panes]")
{
    auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };
    CHECK(charActions(KeyboardPlatformKind::Xkb, ctrlShift, U'W') == Names { "ClosePane" });
    CHECK(charActions(KeyboardPlatformKind::Win32, ctrlShift, U'W') == Names { "ClosePane" });
    CHECK(charActions(KeyboardPlatformKind::Mac, Modifiers { Modifier::Super }, U'W')
          == Names { "ClosePane" });
    CHECK(charActions(KeyboardPlatformKind::Mac, Modifiers { Modifier::Super, Modifier::Shift }, U'W')
          == Names { "CloseTab" });
    // The desktop keeps Ctrl+Shift+X.
    CHECK(charActions(KeyboardPlatformKind::Xkb, ctrlShift, U'X') == Names { "ClosePane" });
}

TEST_CASE("the desktop copies with Ctrl+Shift+C and enlarges the font with Ctrl+Shift+=", "[bindings]")
{
    auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 })
    {
        INFO("platform " << static_cast<int>(kind));
        CHECK(charActions(kind, ctrlShift, U'C') == Names { "CopySelection" });
        CHECK(charActions(kind, ctrlShift, U'=') == Names { "IncreaseFontSize" });
        CHECK(charActions(kind, ctrlShift, U'+') == Names { "IncreaseFontSize" });
        // Copying clears the selection, as Ctrl+C in select mode does.
        auto const* copy = fallbackCharRow(kind, ctrlShift, U'C');
        REQUIRE(copy != nullptr);
        REQUIRE(copy->mapping.binding.size() == 2);
        CHECK(std::holds_alternative<actions::CancelSelection>(copy->mapping.binding.at(1)));
    }
}

TEST_CASE("macOS answers the desktop's Ctrl+Alt chords with Control+Command", "[bindings]")
{
    auto const mac = KeyboardPlatformKind::Mac;
    auto const ctrlCmd = Modifiers { Modifier::Control, Modifier::Super };
    CHECK(charActions(mac, ctrlCmd, U'.') == Names { "ToggleStatusLine" });
    CHECK(charActions(mac, ctrlCmd, U'O') == Names { "OpenFileManager" });
    CHECK(charActions(mac, ctrlCmd, U'S') == Names { "ScreenshotVT" });
    CHECK(charActions(mac, ctrlCmd, U',') == Names { "ToggleInputMethodHandling" });
    CHECK(charActions(mac, ctrlCmd, U'V') == Names { "PasteClipboard" });
    auto const* paste = fallbackCharRow(mac, ctrlCmd, U'V');
    REQUIRE(paste != nullptr);
    CHECK(std::get<actions::PasteClipboard>(paste->mapping.binding.front()).strip);
}

TEST_CASE("macOS opens the last command's output with Shift+Command+O at the prompt only", "[bindings]")
{
    auto const mac = KeyboardPlatformKind::Mac;
    auto const chord = Modifiers { Modifier::Super, Modifier::Shift };
    CHECK(fallbackCharActions(mac, chord, U'O') == Names { "OpenCommandOutput" });
    auto const* row = fallbackCharRow(mac, chord, U'O');
    REQUIRE(row != nullptr);
    CHECK(row->mapping.modes == MatchModes { 0, MatchModes::AlternateScreen });
    CHECK(row->declined == DeclinedInput::Consume);
}

TEST_CASE("the desktop steps between commands as VS Code does", "[bindings]")
{
    using vtbackend::Key;
    auto const prompt = MatchModes { 0, MatchModes::AlternateScreen };
    auto const ctrl = Modifiers { Modifier::Control };
    auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 })
    {
        INFO("platform " << static_cast<int>(kind));
        CHECK(keyActions(kind, ctrl, Key::UpArrow) == Names { "FocusPreviousCommand" });
        CHECK(keyActions(kind, ctrl, Key::DownArrow) == Names { "FocusNextCommand" });
        CHECK(keyActions(kind, ctrlShift, Key::UpArrow) == Names { "SelectPreviousCommandOutput" });
        CHECK(keyActions(kind, ctrlShift, Key::DownArrow) == Names { "SelectNextCommandOutput" });
        // At the prompt only: every row on these chords requires the primary screen, so an alternate-screen
        // application keeps them.
        for (auto const key: { Key::UpArrow, Key::DownArrow })
        {
            CHECK(keyActions(kind, ctrl, key, prompt) == keyActions(kind, ctrl, key));
            CHECK(keyActions(kind, ctrlShift, key, prompt) == keyActions(kind, ctrlShift, key));
        }
        CHECK(fallbackCharActions(kind, ctrlShift, U'L') == Names { "ClearToPrompt" });
        auto const* clear = fallbackCharRow(kind, ctrlShift, U'L');
        REQUIRE(clear != nullptr);
        CHECK(clear->mapping.modes == prompt);
        CHECK(clear->declined == DeclinedInput::Consume);
    }
    // The jump between prompts moved off Ctrl+Shift+Up/Down onto Ctrl+Alt+K/J, which must reach a
    // configuration that predates them, or such a configuration loses the jump altogether.
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 })
    {
        auto const ctrlAlt = Modifiers { Modifier::Control, Modifier::Alt };
        CHECK(fallbackCharActions(kind, ctrlAlt, U'K') == Names { "ScrollMarkUp" });
        CHECK(fallbackCharActions(kind, ctrlAlt, U'J') == Names { "ScrollMarkDown" });
    }
    // macOS keeps Control+Shift+Up/Down as the jump between prompts.
    CHECK(keyActions(KeyboardPlatformKind::Mac, ctrlShift, Key::UpArrow) == Names { "ScrollMarkUp" });
    CHECK(keyActions(KeyboardPlatformKind::Mac, ctrlShift, Key::DownArrow) == Names { "ScrollMarkDown" });
}

TEST_CASE("a configuration generated on one platform binds no chord twice on another", "[bindings][primary]")
{
    // Primary exists so that one contour.yml works on every machine. A generated file whose rows fold onto
    // one chord elsewhere runs both actions there (the loader merges them), e.g. Command+W and
    // Shift+Command+W both reading back as Ctrl+Shift+W, closing the pane and then the whole tab.
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        for (auto const from: AllKinds)
            for (auto const to: AllKinds)
            {
                INFO("edition " << token << ", written on " << static_cast<int>(from) << ", read on "
                                << static_cast<int>(to));
                auto const mappings = carriedMappings(defaultInputMappings(edition, from), from, to);
                INFO(describe(mappings));
                CHECK(findBindingCollisions(mappings).empty());
            }
}

TEST_CASE("portablePrimarySpellings respells whichever row of a folding pair holds Primary",
          "[bindings][primary]")
{
    // Written on a Mac, Command+C (Primary) and a literal Ctrl+Shift+C both read back as Ctrl+Shift+C on the
    // desktop. The later row has no Primary to give up, so the earlier one is written literally.
    auto const rows = std::vector<CharInputMapping> {
        CharInputMapping { .modes = {},
                           .modifiers = Modifiers { Modifier::Super },
                           .input = U'C',
                           .binding = { actions::CopySelection {} } },
        CharInputMapping { .modes = {},
                           .modifiers = Modifiers { Modifier::Control, Modifier::Shift },
                           .input = U'C',
                           .binding = { actions::ToggleFullscreen {} } },
    };
    CHECK(portablePrimarySpellings(std::span<CharInputMapping const> { rows }, KeyboardPlatformKind::Mac)
          == std::vector { PrimarySpelling::Literal, PrimarySpelling::Canonical });
}

namespace
{
/// Whether @p mappings binds the character @p input with @p modifiers to exactly one action, of type @p A.
template <typename A>
bool bindsChar(InputMappings const& mappings, char32_t input, Modifiers modifiers)
{
    return std::ranges::any_of(mappings.charMappings, [&](auto const& mapping) {
        return mapping.input == input && mapping.modifiers == modifiers && mapping.binding.size() == 1
               && std::holds_alternative<A>(mapping.binding.front());
    });
}
} // namespace

TEST_CASE("default chords: Command+\\ and Ctrl+Shift+B toggle the sidebar, Command+J and Ctrl+Shift+J jump, "
          "only in Native2026",
          "[bindings][sidebar]")
{
    auto const& mac = defaultInputMappings(DefaultsEdition::Native2026, KeyboardPlatformKind::Mac);
    auto const& desktop = defaultInputMappings(DefaultsEdition::Native2026, KeyboardPlatformKind::Xkb);
    constexpr auto Command = Modifiers { Modifier::Super };
    constexpr auto CommandShift = Modifiers { Modifier::Super, Modifier::Shift };
    constexpr auto CtrlShift = Modifiers { Modifier::Control, Modifier::Shift };

    CHECK(bindsChar<actions::ToggleSidebar>(mac, U'\\', Command));
    CHECK(bindsChar<actions::JumpToNextAttention>(mac, U'J', Command));
    CHECK(bindsChar<actions::FocusSidebar>(mac, U'E', CommandShift));
    CHECK(bindsChar<actions::ToggleSidebar>(desktop, U'B', CtrlShift));
    CHECK(bindsChar<actions::JumpToNextAttention>(desktop, U'J', CtrlShift));
    CHECK(bindsChar<actions::FocusSidebar>(desktop, U'D', CtrlShift));

    for (auto const kind: AllKinds)
    {
        auto const& classic = defaultInputMappings(DefaultsEdition::Classic, kind);
        CHECK(std::ranges::none_of(classic.charMappings, [](auto const& mapping) {
            auto const& action = mapping.binding.front();
            return std::holds_alternative<actions::ToggleSidebar>(action)
                   || std::holds_alternative<actions::JumpToNextAttention>(action)
                   || std::holds_alternative<actions::FocusSidebar>(action);
        }));
    }
}

TEST_CASE("the sidebar's chords reach a Native2026 configuration written before them, and no Classic one",
          "[bindings][sidebar]")
{
    // A contour.yml written before the sidebar existed lacks its rows; the fallback tables carry them, gated
    // by the defaults edition, since those tables serve every edition.
    auto const sidebarRowsOf = [](KeyboardPlatformKind kind) {
        auto rows = std::vector<FallbackCharMapping> {};
        for (auto const& row: builtinFallbackCharMappings(kind))
        {
            auto const& action = row.mapping.binding.front();
            if (std::holds_alternative<actions::ToggleSidebar>(action)
                || std::holds_alternative<actions::JumpToNextAttention>(action)
                || std::holds_alternative<actions::FocusSidebar>(action))
                rows.push_back(row);
        }
        return rows;
    };
    for (auto const kind: AllKinds)
    {
        INFO("kind " << static_cast<int>(kind));
        auto const rows = sidebarRowsOf(kind);
        CHECK(rows.size() == 3);
        auto const native =
            Config { .defaultsEdition = DefaultsEdition::Native2026, .keyboardPlatformKind = kind };
        auto const classic =
            Config { .defaultsEdition = DefaultsEdition::Classic, .keyboardPlatformKind = kind };
        for (auto const& row: rows)
        {
            CHECK(row.enabled(native));
            CHECK_FALSE(row.enabled(classic));
        }
    }
    constexpr auto CtrlShift = Modifiers { Modifier::Control, Modifier::Shift };
    CHECK(std::ranges::any_of(sidebarRowsOf(KeyboardPlatformKind::Xkb), [&](auto const& row) {
        return row.mapping.input == U'B' && row.mapping.modifiers == CtrlShift
               && std::holds_alternative<actions::ToggleSidebar>(row.mapping.binding.front());
    }));
}
