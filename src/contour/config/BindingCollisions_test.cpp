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
#include <format>
#include <string>
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
