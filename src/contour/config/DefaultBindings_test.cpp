// SPDX-License-Identifier: Apache-2.0
//
// The built-in binding tables, pinned row by row against golden files.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DefaultBindings.hpp>

#include <vtbackend/input/InputBinding.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace contour::config;

namespace
{

/// An action with the arguments that identify it, which formatAction() leaves out where the config
/// writer has no use for them (a paste's strip flag, the characters a SendChars sends).
[[nodiscard]] std::string describeAction(contour::actions::Action const& action)
{
    using namespace contour::actions;
    return std::visit(
        [&](auto const& a) -> std::string {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, PasteClipboard>)
                return std::format("PasteClipboard(strip={})", a.strip ? "yes" : "no");
            else if constexpr (std::is_same_v<T, SendChars>)
                return std::format("SendChars({:?})", a.chars);
            else if constexpr (std::is_same_v<T, HintMode>)
                return std::format("HintMode({}, {})", a.patterns, std::to_underlying(a.hintAction));
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
    return std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
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

TEST_CASE("the Native2026 follow-hyperlink row is the platform's followHyperlinkModifier",
          "[config][defaults]")
{
    for (auto const kind:
         { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32, KeyboardPlatformKind::Mac })
    {
        auto const mappings = makeDefaultInputMappings(DefaultsEdition::Native2026, kind);
        auto const row = std::ranges::find_if(mappings.mouseMappings, [](auto const& have) {
            return !have.binding.empty()
                   && std::holds_alternative<contour::actions::FollowHyperlink>(have.binding.front());
        });
        REQUIRE(row != mappings.mouseMappings.end());
        CHECK(row->modifiers == followHyperlinkModifier(kind));
    }
    // The Classic edition follows with Control everywhere.
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Mac })
    {
        auto const mappings = makeDefaultInputMappings(DefaultsEdition::Classic, kind);
        auto const row = std::ranges::find_if(mappings.mouseMappings, [](auto const& have) {
            return !have.binding.empty()
                   && std::holds_alternative<contour::actions::FollowHyperlink>(have.binding.front());
        });
        REQUIRE(row != mappings.mouseMappings.end());
        CHECK(row->modifiers == vtbackend::Modifiers { vtbackend::Modifier::Control });
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
