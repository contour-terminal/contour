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
    {
        INFO("edition " << token);
        auto const& mappings = defaultInputMappings(edition);
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
#ifdef __APPLE__
    CHECK(binds<actions::FocusToolbar>(native, vtbackend::Key::F5, Modifiers { Modifier::Control }));
#else
    CHECK(std::ranges::none_of(native.keyMappings, [](auto const& mapping) {
        return std::holds_alternative<actions::FocusToolbar>(mapping.binding.front());
    }));
#endif

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
/// Whether @p row is one of @p rows' chords.
template <typename Row, typename Rows>
bool carriesChord(Rows const& rows, Row const& row)
{
    return std::ranges::any_of(rows, [&](auto const& have) {
        return have.input == row.mapping.input && have.modifiers == row.mapping.modifiers;
    });
}

/// The defaults of @p edition with the pane chords of @p host in place of the build host's own.
InputMappings defaultsOnHost(DefaultsEdition edition, PaneChordHost host)
{
    auto mappings = defaultInputMappings(edition);
    for (auto const& row: paneKeyChords(BuildPaneChordHost))
        std::erase_if(mappings.keyMappings, [&](auto const& have) {
            return have.input == row.mapping.input && have.modifiers == row.mapping.modifiers;
        });
    for (auto const& row: paneCharChords(BuildPaneChordHost))
        std::erase_if(mappings.charMappings, [&](auto const& have) {
            return have.input == row.mapping.input && have.modifiers == row.mapping.modifiers;
        });
    for (auto const& row: paneKeyChords(host))
        mappings.keyMappings.push_back(row.mapping);
    for (auto const& row: paneCharChords(host))
        mappings.charMappings.push_back(row.mapping);
    return mappings;
}
} // namespace

TEST_CASE("the pane chords of every host collide with no other default", "[bindings][panes]")
{
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        for (auto const hostIndex: std::views::iota(uint8_t { 0 }, std::to_underlying(PaneChordHost::Count)))
        {
            auto const mappings = defaultsOnHost(edition, static_cast<PaneChordHost>(hostIndex));
            INFO("edition " << token << ", host " << hostIndex);
            CHECK(findBindingCollisions(mappings).empty());
            if (!findBindingCollisions(mappings).empty())
                FAIL_CHECK(describe(mappings));
        }
}

TEST_CASE("every host can focus all four neighbours and split both ways", "[bindings][panes]")
{
    for (auto const hostIndex: std::views::iota(uint8_t { 0 }, std::to_underlying(PaneChordHost::Count)))
    {
        auto const host = static_cast<PaneChordHost>(hostIndex);
        INFO("host " << hostIndex);
        auto const keyActions =
            paneKeyChords(host) | std::views::transform([](auto const& row) { return row.mapping.binding; });
        auto const charActions =
            paneCharChords(host) | std::views::transform([](auto const& row) { return row.mapping.binding; });
        auto const has = [](auto const& range, auto const& wanted) {
            return std::ranges::any_of(range, [&](auto const& list) {
                return list.size() == 1
                       && std::holds_alternative<std::remove_cvref_t<decltype(wanted)>>(list[0]);
            });
        };
        CHECK(has(keyActions, actions::FocusPaneLeft {}));
        CHECK(has(keyActions, actions::FocusPaneRight {}));
        CHECK(has(keyActions, actions::FocusPaneUp {}));
        CHECK(has(keyActions, actions::FocusPaneDown {}));
        CHECK(has(charActions, actions::SplitVertical {}));
        CHECK(has(charActions, actions::SplitHorizontal {}));
    }
}

TEST_CASE("every host moves the active tab with the browsers' chords", "[bindings][panes]")
{
    struct Row
    {
        PaneChordHost host;
        Modifiers modifiers;
    };
    auto const rows = std::array {
        Row { .host = PaneChordHost::Desktop, .modifiers = Modifiers { Modifier::Control, Modifier::Shift } },
        Row { .host = PaneChordHost::Mac, .modifiers = Modifiers { Modifier::Super } },
    };
    for (auto const& [host, modifiers]: rows)
    {
        INFO("host " << std::to_underlying(host));
        auto const actionOf = [&](vtbackend::Key key) {
            auto const keys = paneKeyChords(host);
            auto const found = std::ranges::find_if(keys, [&](auto const& row) {
                return row.mapping.input == key && row.mapping.modifiers == modifiers;
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
        auto const chars = paneCharChords(PaneChordHost::Mac);
        auto const found = std::ranges::find_if(chars, [&](auto const& row) {
            return row.mapping.input == U'D' && row.mapping.modifiers == modifiers;
        });
        REQUIRE(found != chars.end());
        return found->mapping.binding.front();
    };
    CHECK(std::holds_alternative<actions::SplitVertical>(split(superOnly)));
    CHECK(std::holds_alternative<actions::SplitHorizontal>(split(superShift)));
}

TEST_CASE("the build host's pane chords are in the defaults and in the fallbacks", "[bindings][panes]")
{
    auto const& defaults = defaultInputMappings();
    for (auto const& row: paneKeyChords(BuildPaneChordHost))
    {
        CHECK(carriesChord(defaults.keyMappings, row));
        CHECK(std::ranges::any_of(builtinFallbackKeyMappings(), [&](auto const& have) {
            return have.mapping.input == row.mapping.input && have.mapping.modifiers == row.mapping.modifiers;
        }));
    }
    for (auto const& row: paneCharChords(BuildPaneChordHost))
    {
        CHECK(carriesChord(defaults.charMappings, row));
        CHECK(std::ranges::any_of(builtinFallbackCharMappings(), [&](auto const& have) {
            return have.mapping.input == row.mapping.input && have.mapping.modifiers == row.mapping.modifiers;
        }));
    }
}

TEST_CASE("every host opens the Connect picker with its New Window to… chord", "[bindings][connect]")
{
    struct Row
    {
        PaneChordHost host;
        Modifiers modifiers;
    };
    auto const rows = std::array {
        Row { .host = PaneChordHost::Desktop, .modifiers = Modifiers { Modifier::Control, Modifier::Shift } },
        Row { .host = PaneChordHost::Mac, .modifiers = Modifiers { Modifier::Super, Modifier::Shift } },
    };
    for (auto const& [host, modifiers]: rows)
    {
        INFO("host " << std::to_underlying(host));
        auto const chars = paneCharChords(host);
        auto const found = std::ranges::find_if(chars, [&](auto const& row) {
            return row.mapping.input == U'K' && row.mapping.modifiers == modifiers;
        });
        REQUIRE(found != chars.end());
        CHECK(std::holds_alternative<actions::NewWindowTo>(found->mapping.binding.front()));
        // No edition's defaults bind the chord to anything else.
        for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        {
            auto const mappings = defaultsOnHost(edition, host);
            auto const claimed = std::ranges::count_if(mappings.charMappings, [&](auto const& have) {
                return have.input == U'K' && have.modifiers == modifiers;
            });
            CHECK(claimed == 1);
        }
    }
}
