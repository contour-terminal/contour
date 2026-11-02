// SPDX-License-Identifier: Apache-2.0
//
// Pins what today's Qt key path writes to the PTY, row by row (see KeyCharacterizationRows.hpp). No
// behaviour is changed by this file: it is the net the key-pipeline rewrite is held against.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/QtKeyCapture.hpp>
#include <contour/session/KeyCharacterizationRows.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Utils.hpp>

#include <QtGui/QKeyEvent>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
    static auto const passthrough = contour::input::makePassthroughKeyboardLayout();
    return kind == KeyboardPlatformKind::Mac ? *mac : *passthrough;
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

/// Splits @p text at @p separator into the numbers between them; nullopt when any is not a number.
[[nodiscard]] std::optional<std::vector<unsigned>> numbersOf(std::string_view text, char separator)
{
    auto numbers = std::vector<unsigned> {};
    for (auto const part: std::views::split(text, separator))
    {
        auto value = unsigned {};
        auto const field = std::string_view { part.begin(), part.end() };
        auto const [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
        if (ec != std::errc {} || end != field.data() + field.size())
            return std::nullopt;
        numbers.push_back(value);
    }
    return numbers;
}

/// Folds the lock keys the Windows key path samples from the live keyboard into an expectation the table
/// states for toggles that are off: kitty carries them as modifier bits, win32-input-mode as control-key
/// state flags. Pure, so it is tested everywhere although only the Windows run needs it.
/// @param expected The bytes the table expects with CapsLock and NumLock off.
/// @param encoding The encoding the row runs under.
/// @param kittyLockBits The kitty modifier bits of the toggles that are on.
/// @param win32LockFlags The win32-input-mode control-key-state flags of the toggles that are on.
/// @return The bytes to expect with those toggles on.
[[nodiscard]] std::string withLocks(std::string_view expected,
                                    KeyEncoding encoding,
                                    unsigned kittyLockBits,
                                    unsigned win32LockFlags)
{
    auto const isKitty =
        encoding == KeyEncoding::KittyDisambiguate || encoding == KeyEncoding::KittyReportAll;
    if (!expected.starts_with("\033[") || expected.size() < 3)
        return std::string { expected };
    auto const body = expected.substr(2, expected.size() - 3);
    auto const terminator = expected.back();

    if (isKitty && terminator == 'u' && kittyLockBits != 0)
    {
        auto numbers = numbersOf(body, ';');
        if (!numbers || numbers->empty())
            return std::string { expected };
        if (numbers->size() == 1)
            numbers->push_back(1);
        numbers->at(1) += kittyLockBits;
        return std::format("\033[{};{}u", numbers->at(0), numbers->at(1));
    }
    if (encoding == KeyEncoding::Win32InputMode && terminator == '_' && win32LockFlags != 0)
    {
        auto numbers = numbersOf(body, ';');
        if (!numbers || numbers->size() != 6)
            return std::string { expected };
        numbers->at(4) |= win32LockFlags;
        return std::format("\033[{};{};{};{};{};{}_",
                           numbers->at(0),
                           numbers->at(1),
                           numbers->at(2),
                           numbers->at(3),
                           numbers->at(4),
                           numbers->at(5));
    }
    return std::string { expected };
}
/// Resolves the binding the session would run for @p event, the way TerminalSession looks it up: the
/// user's mappings first, then the built-in fallback rows, by named key or else by (folded) character.
/// @param session The session whose live configuration and binding modes decide.
/// @param event The native key event.
/// @return The name of the first action of the binding, or nullopt when nothing binds the chord.
[[nodiscard]] std::optional<std::string> resolvedActionOf(contour::session::TerminalSession const& session,
                                                          contour::input::NativeKeyEvent const& event)
{
    auto const& config = session.config();
    auto const& mappings = config.inputMappings.value();
    auto const flags = session.terminal().bindingModes();
    auto const qtModifiers = Qt::KeyboardModifiers::fromInt(static_cast<int>(event.qtModifiers));
    auto const chord =
        contour::input::makeModifiers(
            qtModifiers, 0, contour::input::ctrlAltRoleOf(qtModifiers, QString::fromStdU32String(event.text)))
            .chord;

    contour::actions::ActionList const* bound = nullptr;
    if (auto const key = contour::input::namedKeyOf(event.qtKey))
    {
        bound = contour::config::apply(mappings.keyMappings, *key, chord, flags);
        if (bound == nullptr)
            if (auto const* row = contour::config::builtinFallbackRow(config, *key, chord, flags))
                bound = &row->mapping.binding;
    }
    else
    {
        auto const lookUp = [&](auto const& find) {
            auto const* found =
                find(contour::config::foldedBindingCodepoint(static_cast<char32_t>(event.qtKey)));
            if (found == nullptr && chord.test(vtbackend::Modifier::Shift))
                if (auto const base = contour::input::unshiftedCodepoint(
                        contour::config::foldedBindingCodepoint(static_cast<char32_t>(event.qtKey)));
                    base != static_cast<char32_t>(event.qtKey))
                    found = find(base);
            return found;
        };
        bound = lookUp(
            [&](char32_t ch) { return contour::config::apply(mappings.charMappings, ch, chord, flags); });
        if (bound == nullptr)
            if (auto const* row = lookUp([&](char32_t ch) -> contour::actions::ActionList const* {
                    auto const* found = contour::config::builtinFallbackRow(config, ch, chord, flags);
                    return found != nullptr ? &found->mapping.binding : nullptr;
                }))
                bound = row;
    }
    if (bound == nullptr || bound->empty())
        return std::nullopt;
    return std::format("{}", bound->front());
}
} // namespace

TEST_CASE("withLocks folds lock toggles into the expectations that carry them", "[input][characterization]")
{
    CHECK(withLocks("\033[117;5u", KeyEncoding::KittyDisambiguate, 128, 0) == "\033[117;133u");
    CHECK(withLocks("\033[186u", KeyEncoding::KittyReportAll, 64, 0) == "\033[186;65u");
    CHECK(withLocks("\033[186u", KeyEncoding::KittyReportAll, 0, 0) == "\033[186u");
    CHECK(withLocks("\033[56;0;91;1;0;1_", KeyEncoding::Win32InputMode, 0, 0x20) == "\033[56;0;91;1;32;1_");
    // Legacy bytes and text are no business of the lock toggles.
    CHECK(withLocks("\033[1;5A", KeyEncoding::Legacy, 128, 0x20) == "\033[1;5A");
    CHECK(withLocks("x", KeyEncoding::KittyDisambiguate, 128, 0x20) == "x");
}

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

TEST_CASE("key characterization: today's Qt path", "[input][characterization]")
{
    for (auto const& row: contour::test::keyCharacterizationRows())
    {
        if (row.kind != contour::config::BuildKeyboardPlatformKind)
            continue;

        DYNAMIC_SECTION(row.name)
        {
            contour::test::TestApp app;
            app.app().config().profile().optionAsAlt.value() = row.optionAsAlt;
            auto pty = std::make_unique<vtpty::MockPty>(
                vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
            auto session =
                contour::test::makeTestSession(&app.app().sessionsManager(), std::move(pty), app.app());
            auto& mockPty = contour::test::mockPtyOf(*session);

            if (row.mode == contour::test::KeyMode::SelectionActive)
                core::locked(session->terminal(), [&] { session->terminal().selectAll(); });

            // A binding is proven by name: the chord must resolve, in the session's live configuration and
            // mode, to the action the row names -- not merely leave the PTY empty.
            if (row.binding == ExpectedBinding::Fires)
            {
                auto const resolved = resolvedActionOf(*session, row.event);
                REQUIRE(resolved.has_value());
                CHECK(*resolved == row.boundAction);
            }

            if (auto const negotiation = negotiationFor(row.encoding); !negotiation.empty())
                session->terminal().writeToScreen(negotiation);

            auto qtEvent = contour::input::toQKeyEvent(row.event);
            mockPty.stdinBuffer().clear();
            contour::session::sendKeyEvent(&qtEvent, row.event.type, *session, layoutFor(row.kind));

            auto expected = std::string { row.expectedPty };
            if constexpr (contour::config::BuildKeyboardPlatformKind == KeyboardPlatformKind::Win32)
            {
                // The key path samples the live CapsLock/NumLock toggles; the table states them off.
                auto const locks = contour::input::makeModifiers(
                                       Qt::NoModifier, contour::input::nativeModifiersWithLockState(0))
                                       .locks;
                auto lockState = vtbackend::Win32ControlKeyState {};
                if (locks.contains(vtbackend::LockKey::CapsLock))
                    lockState.enable(vtbackend::Win32ControlKeyFlag::CapsLockOn);
                if (locks.contains(vtbackend::LockKey::NumLock))
                    lockState.enable(vtbackend::Win32ControlKeyFlag::NumLockOn);
                expected = withLocks(expected, row.encoding, locks.value(), lockState.value());
            }
            CHECK(mockPty.stdinBuffer() == expected);

            // CopySelection is followed by CancelSelection: the selection is gone once the chord has run.
            if (row.mode == contour::test::KeyMode::SelectionActive)
                CHECK_FALSE(core::locked(session->terminal(),
                                         [&] { return session->terminal().selectionAvailable(); }));
        }
    }
}
