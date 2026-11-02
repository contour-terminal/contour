// SPDX-License-Identifier: Apache-2.0
//
// Pins what the key path writes to the PTY, row by row (see KeyCharacterizationRows.hpp): each row's event
// through its own platform's normalizer, the router and the session. It is the net the key-pipeline rewrite
// is held against.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/input/KeyRouter.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/session/KeyCharacterizationRows.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace
{
using contour::config::KeyboardPlatformKind;
using contour::test::ExpectedBinding;
using contour::test::KeyCharacterizationRow;
using contour::test::KeyEncoding;

/// @param kind The platform kind whose layout is wanted.
/// @return The keyboard layout the platform's key path reads native key codes with.
[[nodiscard]] contour::input::KeyboardLayout const& layoutFor(KeyboardPlatformKind kind)
{
    static auto const mac = contour::input::makeMacUsAnsiKeyboardLayout();
    static auto const win32 = contour::input::makeWin32UsKeyboardLayout();
    // The keymap behind the Xkb rows: a US keyboard's number row is all they read.
    static auto const keymapRows =
        std::array { contour::input::FixedXkbKey { .keycode = 12, .group = 0, .keysym = 0x33 } };
    static auto const xkb =
        contour::input::makeXkbKeyboardLayout(contour::input::makeFixedXkbKeymap(keymapRows));
    switch (kind)
    {
        case KeyboardPlatformKind::Mac: return *mac;
        case KeyboardPlatformKind::Win32: return *win32;
        case KeyboardPlatformKind::Xkb: break;
    }
    return *xkb;
}

/// @param encoding The encoding a row runs under.
/// @return The bytes an application writes to ask for it; empty for the legacy encoding.
[[nodiscard]] std::string_view negotiationFor(KeyEncoding encoding) noexcept
{
    switch (encoding)
    {
        case KeyEncoding::Legacy: return {};
        case KeyEncoding::KittyDisambiguate: return "\033[>1u";
        case KeyEncoding::KittyReportAll: return "\033[>9u";
        case KeyEncoding::Win32InputMode: return "\033[?9001h";
    }
    return {};
}

/// Resolves the binding the session would run for @p row: its platform normalizes the event, and the router
/// plans its route in the session's live configuration and binding modes.
/// @param session The session whose live configuration and binding modes decide.
/// @param row The row whose event is routed.
/// @return The name of the first action of the first binding candidate, or nullopt when nothing binds it.
[[nodiscard]] std::optional<std::string> resolvedActionOf(contour::session::TerminalSession const& session,
                                                          KeyCharacterizationRow const& row)
{
    auto const event = session.keyboardPlatform().normalize(row.event);
    auto const context =
        contour::input::RouteContext { .config = session.config(),
                                       .modes = session.terminal().bindingModes(),
                                       .superEncoding = contour::input::SuperEncoding::Unrepresentable };
    auto const route = [&] {
        if (event.key)
            return contour::input::routeKey(*event.key, event.modifiers, event.appModifier, context);
        if (!event.text.empty())
            return contour::input::routeChar(
                event.text.front(), event.identity, event.modifiers, event.appModifier, context);
        return contour::input::KeyRoute {};
    }();
    auto const* bound = route.user.actions != nullptr ? route.user.actions : route.fallback.actions;
    if (bound == nullptr || bound->empty())
        return std::nullopt;
    return std::format("{}", bound->front());
}
} // namespace

TEST_CASE("the characterization table is well formed", "[input][characterization]")
{
    auto names = std::set<std::string_view> {};
    for (auto const& row: contour::test::keyCharacterizationRows())
    {
        INFO(row.name);
        CHECK(names.insert(row.name).second);
        // A binding writes nothing and names its action; a row without one names none.
        CHECK((row.binding == ExpectedBinding::Fires) == !row.boundAction.empty());
        if (row.binding == ExpectedBinding::Fires)
            CHECK(row.expectedPty.empty());
    }
    for (auto const kind:
         { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32, KeyboardPlatformKind::Mac })
        CHECK(
            std::ranges::count(contour::test::keyCharacterizationRows(), kind, &KeyCharacterizationRow::kind)
            > 0);
}

TEST_CASE("key characterization", "[input][characterization]")
{
    for (auto const& row: contour::test::keyCharacterizationRows())
    {
        DYNAMIC_SECTION(row.name)
        {
            contour::test::TestApp app;
            // The row's platform also decides which default bindings the configuration carries.
            auto& config = app.app().config();
            config.keyboardPlatformKind = row.kind;
            config.inputMappings.value() =
                contour::config::makeDefaultInputMappings(config.defaultsEdition, row.kind);
            config.profile().optionAsAlt.value() = row.optionAsAlt;
            auto pty = std::make_unique<vtpty::MockPty>(
                vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
            // The row's platform, whatever the host: every kind's rules run on every machine.
            auto session = contour::test::makeTestSession(
                &app.app().sessionsManager(),
                std::move(pty),
                app.app(),
                contour::input::makeKeyboardPlatformFactory(row.kind, layoutFor(row.kind)));
            auto& mockPty = contour::test::mockPtyOf(*session);

            if (row.mode == contour::test::KeyMode::SelectionActive)
                core::locked(session->terminal(), [&] { session->terminal().selectAll(); });
            if (row.mode == contour::test::KeyMode::AlternateScreen)
                session->terminal().writeToScreen("\033[?1049h");

            // A binding is proven by name: the chord must resolve, in the session's live configuration and
            // mode, to the action the row names -- not merely leave the PTY empty.
            if (row.binding == ExpectedBinding::Fires)
            {
                auto const resolved = resolvedActionOf(*session, row);
                REQUIRE(resolved.has_value());
                CHECK(*resolved == row.boundAction);
            }

            if (auto const negotiation = negotiationFor(row.encoding); !negotiation.empty())
                session->terminal().writeToScreen(negotiation);

            mockPty.stdinBuffer().clear();
            std::ignore = session->sendKeyboardEvent(session->keyboardPlatform().normalize(row.event),
                                                     std::chrono::steady_clock::now());
            CHECK(mockPty.stdinBuffer() == row.expectedPty);

            // CopySelection is followed by CancelSelection: the selection is gone once the chord has run.
            if (row.mode == contour::test::KeyMode::SelectionActive)
                CHECK_FALSE(core::locked(session->terminal(),
                                         [&] { return session->terminal().selectionAvailable(); }));
        }
    }
}
