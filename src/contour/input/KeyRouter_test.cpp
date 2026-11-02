// SPDX-License-Identifier: Apache-2.0
//
// The router on its own: a Config built here, no session, no platform.

#include <contour/config/Config.hpp>
#include <contour/input/KeyRouter.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <variant>

namespace
{
using contour::config::DeclinedInput;
using contour::input::AppModifier;
using contour::input::ConsumeInput;
using contour::input::RouteContext;
using contour::input::SendChar;
using contour::input::SendKey;
using contour::input::SuperEncoding;
using vtbackend::Key;
using vtbackend::KeyboardModifiers;
using vtbackend::Modifier;
using vtbackend::Modifiers;

/// A configuration whose input mappings are exactly @p mappings (the built-in fallback rows still apply).
[[nodiscard]] contour::config::Config configWith(std::string_view mappings)
{
    return contour::test::loadConfigFromYaml(std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
{}
)",
                                                         mappings));
}

/// @return The name of the first action of @p actions, or empty when there are none.
[[nodiscard]] std::string firstActionOf(contour::config::ActionList const* actions)
{
    if (actions == nullptr || actions->empty())
        return {};
    return std::format("{}", actions->front());
}

/// A context in the primary screen, with nothing selected, under @p encoding.
[[nodiscard]] RouteContext contextOf(contour::config::Config const& config,
                                     SuperEncoding encoding = SuperEncoding::Unrepresentable)
{
    return RouteContext { .config = config, .modes = 0, .superEncoding = encoding };
}

} // namespace

TEST_CASE("routeKey finds the user row before the fallback row", "[input][router]")
{
    auto const config = configWith(R"(
    - { mods: [],        key: F3,     action: ToggleFullscreen }
    - { mods: [Control], key: PageUp, action: ScrollPageUp }
)");

    SECTION("a user row alone")
    {
        auto const route = contour::input::routeKey(Key::F3, {}, AppModifier::Absent, contextOf(config));
        CHECK(firstActionOf(route.user.actions) == "ToggleFullscreen");
        CHECK(route.user.declined == DeclinedInput::PassThrough);
        CHECK(route.fallback.actions == nullptr);
        REQUIRE(std::holds_alternative<SendKey>(route.otherwise));
        CHECK(std::get<SendKey>(route.otherwise).key == Key::F3);
    }

    SECTION("a user row and the built-in row of the same chord")
    {
        auto const route = contour::input::routeKey(
            Key::PageUp, Modifiers { Modifier::Control }, AppModifier::Absent, contextOf(config));
        CHECK(firstActionOf(route.user.actions) == "ScrollPageUp");
        CHECK(firstActionOf(route.fallback.actions) == "SwitchToTabLeft");
        CHECK(route.fallback.declined == DeclinedInput::PassThrough);
    }

    SECTION("the fallback row's decline policy travels with it")
    {
        auto const userBound = configWith(R"(
    - { mods: [Control, Shift], key: G, action: ToggleFullscreen }
)");
        auto const route = contour::input::routeChar(U'G',
                                                     {},
                                                     Modifiers { Modifier::Control, Modifier::Shift },
                                                     AppModifier::Absent,
                                                     contextOf(userBound));
        CHECK(firstActionOf(route.user.actions) == "ToggleFullscreen");
        CHECK(route.user.declined == DeclinedInput::PassThrough);
        CHECK(firstActionOf(route.fallback.actions).starts_with("OpenCommandOutput"));
        CHECK(route.fallback.declined == DeclinedInput::Consume);
    }
}

TEST_CASE("routeChar matches bindings case-folded", "[input][router]")
{
    auto const config = configWith(R"(
    - { mods: [Control, Shift], key: 'c', action: ToggleFullscreen }
)");
    auto const ctrlShift = KeyboardModifiers { Modifiers { Modifier::Control, Modifier::Shift } };
    auto const lower = contour::input::routeChar(U'c', {}, ctrlShift, AppModifier::Absent, contextOf(config));
    auto const upper = contour::input::routeChar(U'C', {}, ctrlShift, AppModifier::Absent, contextOf(config));
    REQUIRE(lower.user.actions != nullptr);
    CHECK(lower.user.actions == upper.user.actions);
}

TEST_CASE("routeChar retries a Shift chord under its unshifted base", "[input][router]")
{
    auto const config = configWith(R"(
    - { mods: [Control, Shift], key: ',', action: ToggleFullscreen }
)");
    auto const route = contour::input::routeChar(
        U'<', {}, Modifiers { Modifier::Control, Modifier::Shift }, AppModifier::Absent, contextOf(config));
    CHECK(route.user.actions != nullptr);

    // Without Shift there is no retry: '<' is then a character of its own.
    auto const unshifted = contour::input::routeChar(
        U'<', {}, Modifiers { Modifier::Control }, AppModifier::Absent, contextOf(config));
    CHECK(unshifted.user.actions == nullptr);
}

TEST_CASE("an unbound app-modifier character is consumed only where Super cannot be encoded",
          "[input][router]")
{
    auto const config = configWith("    - { mods: [], key: F3, action: ToggleFullscreen }");
    auto const super = KeyboardModifiers { Modifier::Super };
    auto const identity = vtbackend::KeyIdentity { .unshiftedKey = U'c' };

    auto const unrepresentable = contour::input::routeChar(
        U'c', identity, super, AppModifier::Held, contextOf(config, SuperEncoding::Unrepresentable));
    CHECK(unrepresentable.user.actions == nullptr);
    CHECK(unrepresentable.fallback.actions == nullptr);
    CHECK(std::holds_alternative<ConsumeInput>(unrepresentable.otherwise));

    auto const representable = contour::input::routeChar(
        U'c', identity, super, AppModifier::Held, contextOf(config, SuperEncoding::Representable));
    REQUIRE(std::holds_alternative<SendChar>(representable.otherwise));
    auto const sent = std::get<SendChar>(representable.otherwise);
    CHECK(sent.codepoint == U'c');
    CHECK(sent.identity.unshiftedKey == U'c');
    CHECK(sent.modifiers.chord == Modifiers { Modifier::Super });

    // Super that is no application modifier -- Linux, Windows -- reaches the application as ever.
    auto const absent = contour::input::routeChar(
        U'c', identity, super, AppModifier::Absent, contextOf(config, SuperEncoding::Unrepresentable));
    CHECK(std::holds_alternative<SendChar>(absent.otherwise));
}

TEST_CASE("an unbound app-modifier named key is sent", "[input][router]")
{
    auto const config = configWith("    - { mods: [], key: F3, action: ToggleFullscreen }");
    auto const route = contour::input::routeKey(Key::UpArrow,
                                                Modifiers { Modifier::Super },
                                                AppModifier::Held,
                                                contextOf(config, SuperEncoding::Unrepresentable));
    REQUIRE(std::holds_alternative<SendKey>(route.otherwise));
    CHECK(std::get<SendKey>(route.otherwise).key == Key::UpArrow);
    CHECK(std::get<SendKey>(route.otherwise).modifiers.chord == Modifiers { Modifier::Super });
}
