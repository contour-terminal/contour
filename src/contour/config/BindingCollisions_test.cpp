// SPDX-License-Identifier: Apache-2.0
//
// The default bindings must be unambiguous: no key press may be claimed by two of them, because
// config::apply() runs only the first match and the second would be dead.

#include <contour/config/Actions.hpp>
#include <contour/config/BindingCollisions.hpp>
#include <contour/config/Config.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

using namespace contour;
using namespace contour::config;
using vtbackend::MatchModes;
using vtbackend::Modifier;
using vtbackend::Modifiers;

namespace
{
/// Every keyboard platform kind.
constexpr auto AllKinds =
    std::array { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32, KeyboardPlatformKind::Mac };

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
