// SPDX-License-Identifier: Apache-2.0
//
// The built-in binding tables, pinned row by row against golden files.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DefaultBindings.hpp>
#include <contour/config/ModifierNames.hpp>

#include <vtbackend/input/InputBinding.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace contour::config;

namespace
{

/// Overload set of the actions whose arguments describeAction() spells out; every other action
/// goes through formatAction(). Named non-template functions, so no std::format call sits inside a
/// generic visitor lambda.
[[nodiscard]] std::string describeArguments(contour::actions::PasteClipboard const& a)
{
    return std::format("PasteClipboard(strip={})", a.strip ? "yes" : "no");
}

/// @p text in double quotes with the control characters spelled out, as the golden files have them.
/// Written out by hand: the `{:?}` debug presentation needs libstdc++ 15.
[[nodiscard]] std::string quotedText(std::string_view text)
{
    auto result = std::string { "\"" };
    for (auto const ch: text)
    {
        switch (ch)
        {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F)
                    result +=
                        std::format("\\u{{{:x}}}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                else
                    result += ch;
        }
    }
    result += '"';
    return result;
}

[[nodiscard]] std::string describeArguments(contour::actions::SendChars const& a)
{
    return "SendChars(" + quotedText(a.chars) + ")";
}

[[nodiscard]] std::string describeArguments(contour::actions::HintMode const& a)
{
    return std::format("HintMode({}, {})", a.patterns, std::to_underlying(a.hintAction));
}

/// An action with the arguments that identify it, which formatAction() leaves out where the config
/// writer has no use for them (a paste's strip flag, the characters a SendChars sends).
[[nodiscard]] std::string describeAction(contour::actions::Action const& action)
{
    using namespace contour::actions;
    return std::visit(
        [&](auto const& a) -> std::string {
            if constexpr (requires { describeArguments(a); })
                return describeArguments(a);
            else
                return formatAction(action);
        },
        action);
}

[[nodiscard]] std::string describeActions(ActionList const& actions)
{
    auto result = std::string {};
    for (auto const& action: actions)
    {
        if (!result.empty())
            result += " + ";
        result += describeAction(action);
    }
    return result;
}

template <typename Input>
[[nodiscard]] std::string describeInput(Input const& input)
{
    if constexpr (std::is_same_v<Input, char32_t>)
        return std::format("U+{:04X}", static_cast<uint32_t>(input));
    else
        return std::format("{}", input);
}

template <typename Input>
[[nodiscard]] std::string describeRow(char const* table,
                                      vtbackend::InputBinding<Input, ActionList> const& row,
                                      DeclinedInput declined,
                                      bool gated)
{
    return std::format("{} modes=[{}] mods=[{}] input={} clicks={} -> {} declined={} gated={}\n",
                       table,
                       row.modes,
                       row.modifiers,
                       describeInput(row.input),
                       row.clickCount ? std::to_string(*row.clickCount) : std::string { "-" },
                       describeActions(row.binding),
                       declined == DeclinedInput::Consume ? "consume" : "pass",
                       gated ? "yes" : "no");
}

template <typename Input>
void appendMappings(std::string& out,
                    char const* table,
                    std::vector<vtbackend::InputBinding<Input, ActionList>> const& rows)
{
    for (auto const& row: rows)
        out += describeRow(table, row, DeclinedInput::PassThrough, false);
}

template <typename Input>
void appendFallbacks(std::string& out, char const* table, std::vector<FallbackMapping<Input>> const& rows)
{
    for (auto const& row: rows)
        out += describeRow(table, row.mapping, row.declined, row.enabled != alwaysEnabled);
}

[[nodiscard]] std::string describeDefaults(InputMappings const& mappings, char const* label)
{
    auto out = std::string {};
    out += std::format("# {}\n", label);
    appendMappings(out, "key", mappings.keyMappings);
    appendMappings(out, "char", mappings.charMappings);
    appendMappings(out, "mouse", mappings.mouseMappings);
    return out;
}

[[nodiscard]] std::filesystem::path testdata(std::string_view name)
{
    return std::filesystem::path { CONTOUR_CONFIG_TESTDATA } / name;
}

[[nodiscard]] std::string readGolden(std::string_view name)
{
    auto in = std::ifstream(testdata(name), std::ios::binary);
    REQUIRE(in.good());
    auto text = std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    // A checkout that converted the golden to CRLF must not make every line differ by its CR.
    std::erase(text, '\r');
    return text;
}

/// The tables of @p kind as the golden files spell them: both editions' defaults, then the fallbacks.
[[nodiscard]] std::string describeTables(KeyboardPlatformKind kind)
{
    auto out = std::string {};
    for (auto const edition: { DefaultsEdition::Classic, DefaultsEdition::Native2026 })
        out += describeDefaults(makeDefaultInputMappings(edition, kind),
                                edition == DefaultsEdition::Classic ? "defaults classic"
                                                                    : "defaults native2026");
    out += "# fallbacks\n";
    appendFallbacks(out, "key", builtinFallbackKeyMappings(kind));
    appendFallbacks(out, "char", builtinFallbackCharMappings(kind));
    appendFallbacks(out, "mouse", builtinFallbackMouseMappings(kind));
    return out;
}

/// The lines of @p text, each led by the `# section` it is in, so that a difference names the table.
[[nodiscard]] std::vector<std::string> linesOf(std::string const& text)
{
    auto result = std::vector<std::string> {};
    auto in = std::istringstream { text };
    auto section = std::string {};
    for (auto line = std::string {}; std::getline(in, line);)
    {
        if (line.starts_with("# "))
            section = line.substr(2);
        else
            result.push_back(std::format("[{}] {}", section, line));
    }
    return result;
}

/// The lines of the checked-in difference file @p name.
[[nodiscard]] std::vector<std::string> goldenLines(std::string_view name)
{
    auto result = std::vector<std::string> {};
    auto in = std::istringstream { readGolden(name) };
    for (auto line = std::string {}; std::getline(in, line);)
        result.push_back(line);
    return result;
}

/// The lines of @p from that @p other has no equal line left for, as a multiset difference, in @p from's
/// order.
[[nodiscard]] std::vector<std::string> linesMissingFrom(std::vector<std::string> from,
                                                        std::vector<std::string> other)
{
    auto result = std::vector<std::string> {};
    for (auto& line: from)
    {
        auto const match = std::ranges::find(other, line);
        if (match != other.end())
            other.erase(match);
        else
            result.push_back(std::move(line));
    }
    return result;
}

/// A row of any built-in table, written or fallback.
template <typename Input>
using AnyRow = vtbackend::InputBinding<Input, ActionList>;

/// Every key row @p kind carries: both editions' written defaults, then the fallbacks.
[[nodiscard]] std::vector<AnyRow<vtbackend::Key>> keyRowsOf(KeyboardPlatformKind kind)
{
    auto rows = std::vector<AnyRow<vtbackend::Key>> {};
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        std::ranges::copy(makeDefaultInputMappings(edition, kind).keyMappings, std::back_inserter(rows));
    std::ranges::copy(builtinFallbackKeyMappings(kind) | std::views::transform(&FallbackKeyMapping::mapping),
                      std::back_inserter(rows));
    return rows;
}

/// The character fallback rows of @p kind, as plain bindings.
[[nodiscard]] std::vector<AnyRow<char32_t>> fallbackCharRowsOf(KeyboardPlatformKind kind)
{
    auto rows = std::vector<AnyRow<char32_t>> {};
    std::ranges::copy(builtinFallbackCharMappings(kind)
                          | std::views::transform(&FallbackCharMapping::mapping),
                      std::back_inserter(rows));
    return rows;
}

/// Every character row @p kind carries: both editions' written defaults, then the fallbacks.
[[nodiscard]] std::vector<AnyRow<char32_t>> charRowsOf(KeyboardPlatformKind kind)
{
    auto rows = std::vector<AnyRow<char32_t>> {};
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
        std::ranges::copy(makeDefaultInputMappings(edition, kind).charMappings, std::back_inserter(rows));
    std::ranges::copy(fallbackCharRowsOf(kind), std::back_inserter(rows));
    return rows;
}

/// Whether some row of @p rows binds @p modifiers with @p input.
template <typename Input>
[[nodiscard]] bool isBound(std::vector<AnyRow<Input>> const& rows,
                           vtbackend::Modifiers modifiers,
                           Input input)
{
    return std::ranges::any_of(
        rows, [&](auto const& row) { return row.modifiers == modifiers && row.input == input; });
}

/// Whether every one of @p rows binding @p modifiers with @p input runs @p action alone, and there is one.
template <typename Action, typename Input>
[[nodiscard]] bool isBoundOnlyTo(std::vector<AnyRow<Input>> const& rows,
                                 vtbackend::Modifiers modifiers,
                                 Input input)
{
    auto matching = rows | std::views::filter([&](auto const& row) {
                        return row.modifiers == modifiers && row.input == input;
                    });
    return !std::ranges::empty(matching) && std::ranges::all_of(matching, [](auto const& row) {
        return row.binding.size() == 1 && std::holds_alternative<Action>(row.binding.front());
    });
}

/// Whether the written defaults of every edition of @p kind bind @p modifiers with @p input to @p Action.
template <typename Action, typename Input>
[[nodiscard]] bool isWrittenInEveryEdition(KeyboardPlatformKind kind,
                                           vtbackend::Modifiers modifiers,
                                           Input input)
{
    return std::ranges::all_of(configEnumValues<DefaultsEdition>(),
                               [&](ConfigEnumInfo<DefaultsEdition> const& info) {
                                   auto const mappings = makeDefaultInputMappings(info.value, kind);
                                   if constexpr (std::is_same_v<Input, char32_t>)
                                       return isBoundOnlyTo<Action>(mappings.charMappings, modifiers, input);
                                   else
                                       return isBoundOnlyTo<Action>(mappings.keyMappings, modifiers, input);
                               });
}

/// The platforms whose Primary is Ctrl+Shift.
constexpr auto DesktopKinds = std::array { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 };

} // namespace

TEST_CASE("default bindings on the desktop are unchanged", "[config][defaults]")
{
    auto const golden = readGolden("default-bindings-desktop.txt");
    CHECK(describeTables(KeyboardPlatformKind::Xkb) == golden);
    CHECK(describeTables(KeyboardPlatformKind::Win32) == golden);
}

TEST_CASE("the Mac table is the desktop table with Primary as Command plus the Mac rows",
          "[config][defaults]")
{
    auto const before = linesOf(readGolden("default-bindings-mac-before.txt"));
    auto const after = linesOf(describeTables(KeyboardPlatformKind::Mac));

    // What the platform column changed for macOS, line by line (@see the spec's table of deliberate
    // behaviour changes): the Ctrl+Shift+letter defaults answer to Command, the default rows that needed Alt
    // on a character key are gone, and a row Command already had is not repeated.
    auto const removed = linesMissingFrom(before, after);
    auto const added = linesMissingFrom(after, before);
    INFO("removed:\n"
         << std::ranges::to<std::string>(
                removed | std::views::transform([](auto const& l) { return l + "\n"; }) | std::views::join));
    INFO("added:\n"
         << std::ranges::to<std::string>(added | std::views::transform([](auto const& l) { return l + "\n"; })
                                         | std::views::join));
    CHECK(removed == goldenLines("default-bindings-mac-removed.txt"));
    CHECK(added == goldenLines("default-bindings-mac-added.txt"));
}

TEST_CASE("no macOS default relies on Alt for a character key", "[config][defaults]")
{
    // Option composes characters on macOS, so an Alt+character chord cannot fire there (#2016).
    for (auto const edition: { DefaultsEdition::Classic, DefaultsEdition::Native2026 })
    {
        auto const mappings = makeDefaultInputMappings(edition, KeyboardPlatformKind::Mac);
        CHECK(std::ranges::none_of(mappings.charMappings, [](auto const& row) {
            return row.modifiers.contains(vtbackend::Modifiers { vtbackend::Modifier::Alt });
        }));
        for (auto const& row: builtinFallbackCharMappings(KeyboardPlatformKind::Mac))
            CHECK_FALSE(row.mapping.modifiers.contains(vtbackend::Modifiers { vtbackend::Modifier::Alt }));
    }
}

TEST_CASE("the Native2026 follow-hyperlink row follows with Command on macOS, else Control",
          "[config][defaults]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    auto const followRow = [](auto const& mappings) {
        return std::ranges::find_if(mappings.mouseMappings, [](auto const& have) {
            return !have.binding.empty()
                   && std::holds_alternative<contour::actions::FollowHyperlink>(have.binding.front());
        });
    };
    for (auto const kind:
         { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32, KeyboardPlatformKind::Mac })
    {
        auto const mappings = makeDefaultInputMappings(DefaultsEdition::Native2026, kind);
        auto const row = followRow(mappings);
        REQUIRE(row != mappings.mouseMappings.end());
        CHECK(row->modifiers
              == Modifiers { kind == KeyboardPlatformKind::Mac ? Modifier::Super : Modifier::Control });
    }
    // The Classic edition follows with Control everywhere.
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Mac })
    {
        auto const mappings = makeDefaultInputMappings(DefaultsEdition::Classic, kind);
        auto const row = followRow(mappings);
        REQUIRE(row != mappings.mouseMappings.end());
        CHECK(row->modifiers == Modifiers { Modifier::Control });
    }
}

TEST_CASE("macOS leaves Cut, Save and the font-size Control chords to the application", "[config][defaults]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    for (auto const edition: { DefaultsEdition::Classic, DefaultsEdition::Native2026 })
    {
        auto const mappings = makeDefaultInputMappings(edition, KeyboardPlatformKind::Mac);
        auto const bound = [&](Modifiers modifiers, char32_t input) {
            return std::ranges::any_of(
                       mappings.charMappings,
                       [&](auto const& row) { return row.modifiers == modifiers && row.input == input; })
                   || std::ranges::any_of(
                       builtinFallbackCharMappings(KeyboardPlatformKind::Mac), [&](auto const& row) {
                           return row.mapping.modifiers == modifiers && row.mapping.input == input;
                       });
        };
        CHECK_FALSE(bound(Modifiers { Modifier::Super }, U'X'));
        CHECK_FALSE(bound(Modifiers { Modifier::Super }, U'S'));
        CHECK_FALSE(bound(Modifiers { Modifier::Control }, U'0'));
        CHECK_FALSE(bound(Modifiers { Modifier::Control, Modifier::Shift }, U'-'));
        // ...while the desktop keeps all of them.
        auto const desktop = makeDefaultInputMappings(edition, KeyboardPlatformKind::Xkb);
        CHECK(std::ranges::any_of(desktop.charMappings, [](auto const& row) {
            return row.modifiers == Modifiers { Modifier::Control } && row.input == U'0';
        }));
    }
}

TEST_CASE("no macOS default takes a chord the menu bar owns", "[config][defaults]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    // The cocoa menu bar owns Quit (Command+Q), Settings (Command+,), Hide (Command+H, Option+Command+H)
    // and Minimize (Command+M); they run the same actions, so no default may bind them.
    auto const owned = std::vector<std::pair<Modifiers, char32_t>> {
        { Modifiers { Modifier::Super }, U'Q' }, { Modifiers { Modifier::Super }, U',' },
        { Modifiers { Modifier::Super }, U'H' }, { Modifiers { Modifier::Super, Modifier::Alt }, U'H' },
        { Modifiers { Modifier::Super }, U'M' },
    };
    for (auto const edition: { DefaultsEdition::Classic, DefaultsEdition::Native2026 })
    {
        auto const mappings = makeDefaultInputMappings(edition, KeyboardPlatformKind::Mac);
        for (auto const& [modifiers, input]: owned)
        {
            CHECK_FALSE(std::ranges::any_of(mappings.charMappings, [&](auto const& row) {
                return row.modifiers == modifiers && row.input == input;
            }));
            CHECK_FALSE(std::ranges::any_of(
                builtinFallbackCharMappings(KeyboardPlatformKind::Mac), [&](auto const& row) {
                    return row.mapping.modifiers == modifiers && row.mapping.input == input;
                }));
        }
        // ...while the desktop keeps Ctrl+Shift+Q.
        auto const desktop = makeDefaultInputMappings(edition, KeyboardPlatformKind::Xkb);
        CHECK(std::ranges::any_of(desktop.charMappings, [](auto const& row) {
            return row.input == U'Q' && row.modifiers == Modifiers { Modifier::Control, Modifier::Shift };
        }));
    }
}

TEST_CASE("no macOS default takes a Control+Command chord the system owns", "[config][defaults]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    // macOS owns Control+Command+Q (lock screen), D (look up) and Space (character viewer). It owns F (full
    // screen) too, and a default may bind that one to what the system means by it, ToggleFullscreen, only.
    auto const controlCommand = Modifiers { Modifier::Control, Modifier::Super };
    auto const owned = std::array { U'Q', U'D', U' ' };
    auto const rows = charRowsOf(KeyboardPlatformKind::Mac);
    for (auto const input: owned)
        CHECK_FALSE(isBound(rows, controlCommand, input));
    for (auto const& row: rows)
        if (row.modifiers == controlCommand && row.input == U'F')
            CHECK((row.binding.size() == 1
                   && std::holds_alternative<contour::actions::ToggleFullscreen>(row.binding.front())));
}

TEST_CASE("no default binds Shift+Left/Right", "[config][defaults]")
{
    // Shift+Left/Right selects a word in an editor and a line editor; they switched tabs once.
    using vtbackend::Key;
    auto const shift = vtbackend::Modifiers { vtbackend::Modifier::Shift };
    for (auto const kind: AllKeyboardPlatformKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        auto const rows = keyRowsOf(kind);
        CHECK_FALSE(isBound(rows, shift, Key::LeftArrow));
        CHECK_FALSE(isBound(rows, shift, Key::RightArrow));
    }
}

TEST_CASE("Shift scrolling rows require the primary screen", "[config][defaults]")
{
    // An alternate-screen application (vim, less, htop) keeps Shift+arrows, Shift+PageUp and the rest.
    using vtbackend::Key;
    auto const shift = vtbackend::Modifiers { vtbackend::Modifier::Shift };
    auto const primaryScreenOnly = vtbackend::MatchModes { 0, vtbackend::MatchModes::AlternateScreen };
    for (auto const kind: AllKeyboardPlatformKinds)
        for (auto const key:
             { Key::UpArrow, Key::DownArrow, Key::PageUp, Key::PageDown, Key::Home, Key::End })
        {
            INFO(std::format("platform {}, key {}", std::to_underlying(kind), key));
            auto const rows = keyRowsOf(kind);
            CHECK(isBound(rows, shift, key));
            for (auto const& row: rows)
                if (row.modifiers == shift && row.input == key)
                    CHECK(row.modes == primaryScreenOnly);
        }
}

TEST_CASE("fullscreen is F11 on the desktop and Control+Command+F on macOS, and Alt+Enter is unbound",
          "[config][defaults]")
{
    using contour::actions::ToggleFullscreen;
    using vtbackend::Key;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    for (auto const kind: AllKeyboardPlatformKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        CHECK_FALSE(isBound(keyRowsOf(kind), Modifiers { Modifier::Alt }, Key::Enter));
    }
    for (auto const kind: DesktopKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        CHECK(isWrittenInEveryEdition<ToggleFullscreen>(kind, Modifiers {}, Key::F11));
    }
    auto const controlCommand = Modifiers { Modifier::Control, Modifier::Super };
    CHECK(isWrittenInEveryEdition<ToggleFullscreen>(KeyboardPlatformKind::Mac, controlCommand, U'F'));
    CHECK(isBoundOnlyTo<ToggleFullscreen>(charRowsOf(KeyboardPlatformKind::Mac), controlCommand, U'F'));
    // F11 is the Mac's Show Desktop.
    CHECK_FALSE(isBound(keyRowsOf(KeyboardPlatformKind::Mac), Modifiers {}, Key::F11));
}

TEST_CASE("font reset is Ctrl+0 on the desktop and Command+0 on macOS, never Ctrl+Shift+0",
          "[config][defaults]")
{
    // Ctrl+Shift+0 cannot reset: on German, Nordic, Spanish, Italian, Portuguese and Turkish-Q layouts
    // Shift+0 types '=', which the router finds first, so the press is Ctrl+Shift+= there and enlarges the
    // font.
    using contour::actions::ResetFontSize;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    for (auto const kind: DesktopKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        CHECK(isWrittenInEveryEdition<ResetFontSize>(kind, Modifiers { Modifier::Control }, U'0'));
    }
    auto const command = Modifiers { Modifier::Super };
    CHECK(isWrittenInEveryEdition<ResetFontSize>(KeyboardPlatformKind::Mac, command, U'0'));
    CHECK(isBoundOnlyTo<ResetFontSize>(fallbackCharRowsOf(KeyboardPlatformKind::Mac), command, U'0'));
    for (auto const kind: AllKeyboardPlatformKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        CHECK_FALSE(isBound(charRowsOf(kind), Modifiers { Modifier::Control, Modifier::Shift }, U'0'));
    }
}

TEST_CASE("Ctrl+Shift+X is unbound on the desktop", "[config][defaults]")
{
    // Terminator zooms a pane with it; Ctrl+Shift+W closes one.
    for (auto const kind: DesktopKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        CHECK_FALSE(isBound(charRowsOf(kind),
                            vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                            U'X'));
    }
}

TEST_CASE("OpenConfiguration is the menu's Command+comma alone on macOS", "[config][defaults]")
{
    // Command+, is the menu bar's Settings, which runs OpenConfiguration itself. Control+Shift+comma, the
    // desktop chord's literal translation, is not bound there, nor is Option+Command+comma: Option composes
    // the comma into another character (no macOS default relies on Alt for a character key). The desktop
    // keeps Ctrl+Shift+comma.
    using contour::actions::OpenConfiguration;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    auto const isOpenConfiguration = [](auto const& row) {
        return std::ranges::any_of(row.binding, [](auto const& action) {
            return std::holds_alternative<OpenConfiguration>(action);
        });
    };
    CHECK(std::ranges::none_of(charRowsOf(KeyboardPlatformKind::Mac), isOpenConfiguration));
    CHECK(std::ranges::none_of(keyRowsOf(KeyboardPlatformKind::Mac), isOpenConfiguration));
    CHECK_FALSE(isBound(
        charRowsOf(KeyboardPlatformKind::Mac), Modifiers { Modifier::Control, Modifier::Shift }, U','));
    for (auto const kind: DesktopKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        CHECK(isWrittenInEveryEdition<OpenConfiguration>(
            kind, Modifiers { Modifier::Control, Modifier::Shift }, U','));
    }
}

TEST_CASE("URL hints are Primary+Y and Primary+U is unbound", "[config][defaults]")
{
    // IBus takes Ctrl+Shift+U for its Unicode entry before the terminal sees it.
    using contour::actions::HintMode;
    for (auto const kind: AllKeyboardPlatformKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        auto const primary = primaryModifiers(kind);
        CHECK(isWrittenInEveryEdition<HintMode>(kind, primary, U'Y'));
        CHECK_FALSE(isBound(charRowsOf(kind), primary, U'U'));
    }
}

TEST_CASE("no default binds ScrollMarkUp/Down", "[config][defaults]")
{
    // FocusPreviousCommand and FocusNextCommand step from prompt to prompt instead.
    auto const isScrollMark = [](auto const& row) {
        return std::ranges::any_of(row.binding, [](auto const& action) {
            return std::holds_alternative<contour::actions::ScrollMarkUp>(action)
                   || std::holds_alternative<contour::actions::ScrollMarkDown>(action);
        });
    };
    for (auto const kind: AllKeyboardPlatformKinds)
    {
        INFO(std::format("platform {}", std::to_underlying(kind)));
        CHECK(std::ranges::none_of(keyRowsOf(kind), isScrollMark));
        CHECK(std::ranges::none_of(charRowsOf(kind), isScrollMark));
    }
}

TEST_CASE("write the golden files", "[.golden-write]")
{
    {
        auto out = std::ofstream(testdata("default-bindings-desktop.txt"), std::ios::binary);
        out << describeTables(KeyboardPlatformKind::Xkb);
    }
    {
        auto const before = linesOf(readGolden("default-bindings-mac-before.txt"));
        auto const after = linesOf(describeTables(KeyboardPlatformKind::Mac));
        auto removed = std::ofstream(testdata("default-bindings-mac-removed.txt"), std::ios::binary);
        for (auto const& line: linesMissingFrom(before, after))
            removed << line << '\n';
        auto added = std::ofstream(testdata("default-bindings-mac-added.txt"), std::ios::binary);
        for (auto const& line: linesMissingFrom(after, before))
            added << line << '\n';
    }
}
