// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/FileOpenerTemplate.hpp>
#include <contour/config/GlobalOverrides.hpp>
#include <contour/config/HostUrl.hpp>
#include <contour/config/ModifierNames.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/input/ClickCounter.hpp>
#include <vtbackend/screen/GutterSettings.hpp>

#include <vtpty/ImageSize.hpp>

#include <text_shaper/Font.hpp>

#include <crispy/StrongHash.hpp>

#include <core/Environment.hpp>
#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/platform/PathUtils.hpp>

#include <libunicode/convert.h>

#include <yaml-cpp/emitter.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <iostream>
#include <ranges>

#ifdef _WIN32
    #include <Windows.h>
#elifdef __APPLE__
    #include <unistd.h>

    #include <mach-o/dyld.h>
#else
    #include <unistd.h>
#endif

auto constexpr MinimumFontSize = text::FontSize { 8.0 };

using namespace std;
using core::homeResolvedPath;
using core::replaceVariables;

using vtpty::Process;

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::Width;

using vtbackend::CellRGBColorAndAlphaPair;
using vtbackend::ColumnCount;
using vtbackend::Infinite;
using vtbackend::LineCount;
using vtbackend::PageSize;

using contour::actions::Action;
using std::string;

using UsedKeys = set<string>;

namespace fs = std::filesystem;

namespace contour::config
{

namespace
{
    /// The modes of a built-in row that acts on command blocks, which exist on the primary screen only.
    constexpr auto PrimaryScreenOnly = vtbackend::MatchModes { 0, vtbackend::MatchModes::AlternateScreen };
} // namespace

std::vector<FallbackMouseMapping> const& builtinFallbackMouseMappings()
{
    // These are fallbacks rather than defaults (see the declaration for why): every contour.yml written
    // before they existed enumerates the default mouse mappings without them, and would otherwise shadow
    // them away.
    //
    // Nothing else is needed to get the precedence right. TerminalSession::sendMousePressEvent consults
    // this table only AFTER vtbackend has declined the press, so an application that asked for the mouse
    // (vim, tmux) still receives its right-click and its horizontal wheel; and only after the bypass
    // modifier has been stripped, so Shift+Right opens the menu and Shift+wheel switches tabs even then.
    // That is the same gate that already decides whether the user may select cells with the mouse —
    // reused, not restated.
    //
    // A row naming a click count is looked up earlier, by the claimant TerminalSession hands the terminal
    // with each press: it stands in for the terminal's own multi-click selection, so it is decided before
    // that selection acts. It is held to the same gate all the same -- the terminal offers a press to the
    // claimant only when its own selection would have seen it.
    //
    // The horizontal-wheel rows match Modifiers{None} deliberately: sendWheelEvent() transposes the wheel
    // axes while Alt is held (see helper.cpp, transposed()), so Alt+vertical wheel arrives here on the
    // horizontal axis. Carrying Alt, it cannot match these rows, and the Alt+wheel opacity bindings keep
    // working untouched.
    static auto const mappings = std::vector<FallbackMouseMapping> {
        FallbackMouseMapping { .mapping = { .modes { vtbackend::MatchModes {} },
                                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                                            .input = vtbackend::MouseButton::Right,
                                            .binding = { { actions::OpenContextMenu {} } } } },
        FallbackMouseMapping {
            .mapping = { .modes { vtbackend::MatchModes {} },
                         .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                         .input = vtbackend::MouseButton::WheelLeft,
                         .binding = { { actions::SwitchToTabLeft {} } } },
            .enabled =
                [](Config const& config) noexcept { return config.tabSwitchOnHorizontalWheel.value(); } },
        FallbackMouseMapping {
            .mapping = { .modes { vtbackend::MatchModes {} },
                         .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                         .input = vtbackend::MouseButton::WheelRight,
                         .binding = { { actions::SwitchToTabRight {} } } },
            .enabled =
                [](Config const& config) noexcept { return config.tabSwitchOnHorizontalWheel.value(); } },
        // Ctrl+triple-click selects the output of the command under the pointer, where the terminal would
        // have selected an extended word. On the primary screen only: the alternate screen has no blocks,
        // so there the row would claim the click only to refuse it, and the extended word would be lost.
        FallbackMouseMapping {
            .mapping = { .modes { PrimaryScreenOnly },
                         .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                         .input = vtbackend::MouseButton::Left,
                         .binding = { { actions::SelectCommandBlock {
                             .target = vtbackend::CommandBlockTarget::Pointer,
                             .part = vtbackend::CommandBlockPart::Output } } },
                         .clickCount = uint8_t { 3 } } },
    };
    return mappings;
}

std::vector<FallbackKeyMapping> const& builtinFallbackKeyMappings()
{
    // Browser-style tab switching. Fallbacks rather than defaults for the reason given on the
    // declaration: every contour.yml written before these existed enumerates the key mappings without
    // them, and would otherwise shadow them away forever.
    //
    // Ctrl+Tab is claimed from the application deliberately, matching Windows Terminal, GNOME Terminal
    // and Konsole. A terminal application CAN ask for it -- the Kitty keyboard protocol gives Ctrl+Tab
    // its own CSI-u sequence -- so this is a real trade, made the way the reference terminals make it.
    // Anyone who needs it in an application binds it in their own input_mapping:, which is consulted
    // first and therefore wins. Should that trade ever need softening, the cheap retreat is a `.modes`
    // with AlternateScreen DISABLED on the two Tab rows: full-screen applications would keep Ctrl+Tab
    // while the shell still switched tabs, using only data that already exists.
    static auto const mappings = std::vector<FallbackKeyMapping> {
        FallbackKeyMapping {
            .mapping = { .modes { vtbackend::MatchModes {} },
                         .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                         .input = vtbackend::Key::PageUp,
                         .binding = { { actions::SwitchToTabLeft {} } } } },
        FallbackKeyMapping {
            .mapping = { .modes { vtbackend::MatchModes {} },
                         .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                         .input = vtbackend::Key::PageDown,
                         .binding = { { actions::SwitchToTabRight {} } } } },
        FallbackKeyMapping {
            .mapping = { .modes { vtbackend::MatchModes {} },
                         .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                         .input = vtbackend::Key::Tab,
                         .binding = { { actions::SwitchToTabRight {} } } } },
        // Ctrl+Shift+Tab arrives as Key::Tab with Shift re-added: Qt reports the shifted press as
        // Key_Backtab, which helper.cpp rewrites (see makeKey).
        FallbackKeyMapping { .mapping = { .modes { vtbackend::MatchModes {} },
                                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control,
                                                                              vtbackend::Modifier::Shift } },
                                          .input = vtbackend::Key::Tab,
                                          .binding = { { actions::SwitchToTabLeft {} } } } },
        // From one command's prompt to the next, as Ghostty's jump_to_prompt binds it. Bound nowhere in the
        // defaults when it was added -- Shift alone scrolls a line, Alt+Shift focuses a pane, Ctrl+Alt swaps
        // one, Ctrl+Shift+Alt resizes one. On the primary screen only, where the prompts are; an
        // alternate-screen application keeps the chord.
        FallbackKeyMapping { .mapping = { .modes { PrimaryScreenOnly },
                                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control,
                                                                              vtbackend::Modifier::Shift } },
                                          .input = vtbackend::Key::UpArrow,
                                          .binding = { { actions::ScrollMarkUp {} } } } },
        FallbackKeyMapping { .mapping = { .modes { PrimaryScreenOnly },
                                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control,
                                                                              vtbackend::Modifier::Shift } },
                                          .input = vtbackend::Key::DownArrow,
                                          .binding = { { actions::ScrollMarkDown {} } } } },
    };
    return mappings;
}

std::vector<FallbackCharMapping> const& builtinFallbackCharMappings()
{
    // Fallbacks rather than defaults for the reason the key table gives. Ctrl+Shift+G was checked to be
    // bound nowhere in the defaults, on any platform, when it was added; it is kitty's binding for the
    // same thing.
    //
    // Consumed when it declines (nothing finished, blank output, an attached session), as kitty does: the
    // shell would read it as ^G, which makes readline drop the line being typed. On the primary screen
    // only, where the blocks are; an alternate-screen application keeps the chord.
    static auto const mappings = std::vector<FallbackCharMapping> {
        FallbackCharMapping { .mapping = { .modes { PrimaryScreenOnly },
                                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control,
                                                                               vtbackend::Modifier::Shift } },
                                           .input = U'G',
                                           .binding = { { actions::OpenCommandOutput {} } } },
                              .declined = DeclinedInput::Consume },
    };
    return mappings;
}

namespace
{
    /// The first enabled row of @p table that matches @p input.
    ///
    /// Shared by every built-in lookup so the gating pipeline -- filter by the row's predicate, then
    /// match -- exists once rather than once per input kind.
    template <typename Input>
    [[nodiscard]] FallbackMapping<Input> const* matchFallbackTable(
        std::vector<FallbackMapping<Input>> const& table,
        Config const& config,
        Input input,
        vtbackend::Modifiers modifiers,
        uint8_t actualModeFlags,
        std::optional<uint8_t> clickCount)
    {
        for (auto const& row: table)
            if (row.enabled(config)
                && apply(std::span { &row.mapping, 1 }, input, modifiers, actualModeFlags, clickCount)
                       != nullptr)
                return &row;
        return nullptr;
    }

    /// The actions of the first enabled row of @p table that matches @p input.
    template <typename Input>
    [[nodiscard]] ActionList const* applyFallbackTable(std::vector<FallbackMapping<Input>> const& table,
                                                       Config const& config,
                                                       Input input,
                                                       vtbackend::Modifiers modifiers,
                                                       uint8_t actualModeFlags,
                                                       std::optional<uint8_t> clickCount)
    {
        auto const* row = matchFallbackTable(table, config, input, modifiers, actualModeFlags, clickCount);
        return row != nullptr ? &row->mapping.binding : nullptr;
    }
} // namespace

ActionList const* applyBuiltinFallback(Config const& config,
                                       vtbackend::MouseButton button,
                                       vtbackend::Modifiers modifiers,
                                       uint8_t actualModeFlags,
                                       std::optional<uint8_t> clickCount)
{
    return applyFallbackTable(
        builtinFallbackMouseMappings(), config, button, modifiers, actualModeFlags, clickCount);
}

ActionList const* applyBuiltinFallback(Config const& config,
                                       vtbackend::Key key,
                                       vtbackend::Modifiers modifiers,
                                       uint8_t actualModeFlags)
{
    return applyFallbackTable(
        builtinFallbackKeyMappings(), config, key, modifiers, actualModeFlags, std::nullopt);
}

ActionList const* applyBuiltinFallback(Config const& config,
                                       char32_t codepoint,
                                       vtbackend::Modifiers modifiers,
                                       uint8_t actualModeFlags)
{
    return applyFallbackTable(
        builtinFallbackCharMappings(), config, codepoint, modifiers, actualModeFlags, std::nullopt);
}

FallbackKeyMapping const* builtinFallbackRow(Config const& config,
                                             vtbackend::Key key,
                                             vtbackend::Modifiers modifiers,
                                             uint8_t actualModeFlags)
{
    return matchFallbackTable(
        builtinFallbackKeyMappings(), config, key, modifiers, actualModeFlags, std::nullopt);
}

FallbackCharMapping const* builtinFallbackRow(Config const& config,
                                              char32_t codepoint,
                                              vtbackend::Modifiers modifiers,
                                              uint8_t actualModeFlags)
{
    return matchFallbackTable(
        builtinFallbackCharMappings(), config, codepoint, modifiers, actualModeFlags, std::nullopt);
}

namespace
{

    auto const configLog = core::log::Category("config", "Logs configuration file loading.");

    /// This translation unit's one environment reader.
    ///
    /// A core::LiveEnvironment, constructed on the spot rather than the process-wide cached one:
    /// `${VAR}` expansion has to see the environment as it stands when the config is read, not as it
    /// stood when the process started. The live reader is nonetheless thread safe, which is what
    /// rules out a bare getenv() here -- config (re)load runs while the PTY threads are live.
    ///
    /// What it answers with is raw bytes rather than decoded text, because what is read here names
    /// filesystem paths and a path is a byte string that has to survive intact -- a mangled one
    /// simply does not exist, and contour then silently falls back to its defaults as though the
    /// user's configuration had never been there.
    ///
    /// @return A reference to a function-local static; it outlives every caller.
    [[nodiscard]] core::Environment const& configEnvironment()
    {
        static core::LiveEnvironment const instance;
        return instance;
    }

    /// Reads an environment variable, treating "unset" and "empty" alike.
    ///
    /// @param name Name of the variable to read.
    /// @param env Environment to read from.
    /// @return The variable's value as raw bytes -- UTF-8 on Windows -- or an empty string if it is unset.
    [[nodiscard]] std::string environmentBytes(std::string_view name, core::Environment const& env)
    {
        return env.get(name).value_or("");
    }

    /// Parses a resize/direction keyword ("Left"/"Right"/"Up"/"Down", case-insensitive) into the
    /// action-layer Direction enum.
    /// @param name The direction keyword from the config.
    /// @return The matching Direction, or nullopt if @p name is not a known direction.
    std::optional<actions::Direction> parseResizeDirection(std::string const& name)
    {
        // Data-driven: one row per direction keyword. Adding a synonym is adding a row.
        using Row = std::pair<std::string_view, actions::Direction>;
        static constexpr std::array<Row, 4> Mapping { {
            { "left", actions::Direction::Left },
            { "right", actions::Direction::Right },
            { "up", actions::Direction::Up },
            { "down", actions::Direction::Down },
        } };
        auto const lowered = core::toLower(name);
        for (auto const& [keyword, direction]: Mapping)
            if (keyword == lowered)
                return direction;
        return std::nullopt;
    }

    /// Renders the modifier spellings a `mods:` entry accepts, for a diagnostic.
    ///
    /// Folded from ConfigModifierTable rather than restated, so a spelling added there shows up in
    /// the error message for free.
    /// @return e.g. "Shift, Alt, Control, Ctrl, Super, Meta, Hyper".
    [[nodiscard]] std::string acceptedModifierSpellings()
    {
        return ConfigModifierTable | std::views::transform(&ConfigModifierRow::name)
               | core::views::joinWith(", ");
    }

    /// Renders an `input_mapping` row for a diagnostic, e.g. "action: ClearHistoryAndReset, key: Q".
    ///
    /// Scalar fields only: an absent or non-scalar field is skipped, so a message explaining a
    /// malformed document cannot itself throw on that document.
    /// @param row The mapping node.
    /// @return The row's recognized fields, or "<empty>" when none are usable.
    [[nodiscard]] std::string describeInputMappingRow(YAML::Node const& row)
    {
        using namespace std::string_view_literals;
        static constexpr auto Fields =
            std::array { "action"sv, "key"sv, "mouse"sv, "clicks"sv, "mods"sv, "mode"sv };

        auto described = std::vector<std::string> {};
        for (auto const field: Fields)
            if (auto const value = row[std::string { field }]; value && value.IsScalar())
                described.emplace_back(std::format("{}: {}", field, value.as<std::string>()));

        if (described.empty())
            return "<empty>";
        return described | core::views::joinWith(", ");
    }

    /// The buttons a `clicks:` binding may name, for a diagnostic: "Left, Middle and Right".
    [[nodiscard]] std::string countedButtonNames()
    {
        auto names = std::ranges::to<std::vector>(
            vtbackend::ClickCounter::CountedButtons
            | std::views::transform([](auto button) { return std::format("{}", button); }));
        auto const last = std::move(names.back());
        names.pop_back();
        return std::format("{} and {}", names | core::views::joinWith(", "), last);
    }

    /// The `clicks:` field of an input_mapping row: the multi-click count its mouse button fires on.
    /// @param row The mapping node.
    /// @return No count when the row names none, the count when it is 1..MaxClickCount, and otherwise
    ///         what a diagnostic says about the field.
    [[nodiscard]] std::expected<std::optional<uint8_t>, std::string> parseClickCount(YAML::Node const& row)
    {
        auto const node = row["clicks"];
        if (!node)
            return std::optional<uint8_t> {};

        auto const count =
            core::toInteger<10, uint8_t>(node.IsScalar() ? node.as<std::string>() : std::string {});
        if (!count || *count < 1 || *count > vtbackend::ClickCounter::MaxClickCount)
            return std::unexpected(
                std::format("clicks (expected 1 to {})", vtbackend::ClickCounter::MaxClickCount));
        return count;
    }

    optional<std::string> readFile(fs::path const& path)
    {
        if (!fs::exists(path))
            return nullopt;

        // Binary mode is mandatory here: the read below is sized by fs::file_size(), so any text-mode
        // CRLF->LF translation (Windows) would make read() fall short of that byte count and leave a
        // trailing NUL that corrupts the parse.
        auto ifs = ifstream(path, std::ios::binary);
        if (!ifs.good())
            return nullopt;

        auto const size = fs::file_size(path);
        auto text = string {};
        text.resize(size);
        ifs.read(text.data(), static_cast<std::streamsize>(size));

        // Normalize CRLF -> LF. Files saved by Windows editors carry '\r' bytes that our YAML parser
        // keeps as trailing characters on plain scalar values ("true\r"), which then fail typed
        // conversion. Stripping them makes side files parse identically regardless of how they were
        // saved; every caller here consumes the result as text, so this is always safe.
        std::erase(text, '\r');
        return { text };
    }

    std::vector<fs::path> configHomes(string const& programName)
    {
        std::vector<fs::path> paths;

#if defined(CONTOUR_PROJECT_SOURCE_DIR) && !defined(NDEBUG)
        paths.emplace_back(fs::path(CONTOUR_PROJECT_SOURCE_DIR) / "src" / "contour" / "display" / "shaders");
#endif

        paths.emplace_back(configHome(programName));

#if defined(__unix__) || defined(__APPLE__)
        paths.emplace_back(fs::path("/etc") / programName);
#endif

        return paths;
    }

    void createFileIfNotExists(fs::path const& path)
    {
        if (!fs::is_regular_file(path))
            if (auto const ec = createDefaultConfig(path); ec)
                throw runtime_error { std::format("Could not create directory {}. {}",
                                                  core::pathToUtf8(path.parent_path()),
                                                  ec.message()) };
    }

#ifndef _WIN32
    std::vector<fs::path> getTermInfoDirs(optional<fs::path> const& appTerminfoDir)
    {
        auto locations = std::vector<fs::path>();

        if (appTerminfoDir.has_value())
            locations.emplace_back(appTerminfoDir.value().string());

        locations.emplace_back(Process::homeDirectory() / ".terminfo");

        if (auto const terminfoDirs = environmentBytes("TERMINFO_DIRS", configEnvironment());
            !terminfoDirs.empty())
            for (auto const dir: core::split(string_view(terminfoDirs), ':'))
                locations.emplace_back(string(dir));

        locations.emplace_back("/usr/share/terminfo");

        // BSD locations
        locations.emplace_back("/usr/local/share/terminfo");
        locations.emplace_back("/usr/local/share/site-terminfo");

        return locations;
    }
#endif

    // Windows has no terminfo database to search, so the directory goes unread there.
    string getDefaultTERM([[maybe_unused]] optional<fs::path> const& appTerminfoDir)
    {
#ifdef _WIN32
        return "contour";
#else

        if (Process::isFlatpak())
            return "contour";

        auto locations = getTermInfoDirs(appTerminfoDir);
        auto const terms = std::vector<string> {
            "contour", "xterm-256color", "xterm", "vt340", "vt220",
        };

        for (auto const& prefix: locations)
            for (auto const& term: terms)
            {
                if (access((prefix / term.substr(0, 1) / term).string().c_str(), R_OK) == 0)
                    return term;

    #ifdef __APPLE__
                // I realized that on Apple the `tic` command sometimes installs
                // the terminfo files into weird paths.
                if (access((prefix / std::format("{:02X}", term.at(0)) / term).string().c_str(), R_OK) == 0)
                    return term;
    #endif
            }

        return "vt100";
#endif
    }

} // namespace

fs::path configHome(string const& programName)
{
#if defined(__unix__) || defined(__APPLE__)
    if (auto const value = environmentBytes("XDG_CONFIG_HOME", configEnvironment()); !value.empty())
        return fs::path { value } / programName;
    else
        return Process::homeDirectory() / ".config" / programName;
#endif

#ifdef _WIN32
    if (auto const home = core::pathFromUtf8(environmentBytes("LOCALAPPDATA", configEnvironment()));
        home && !home->empty())
        return *home / programName;
    throw runtime_error { "Could not find config home folder." };
#endif
}

fs::path configHome()
{
    return configHome("contour");
}

static std::string createString(Config const& c)
{
    return createString<YAMLConfigWriter>(c);
}

namespace
{
    /// The font configuration @p edition ships.
    /// @param edition The edition whose font to describe.
    /// @return Every face in the edition's family, at its size.
    [[nodiscard]] vtrasterizer::FontDescriptions makeDefaultFont(DefaultsEdition edition)
    {
        auto const& defaults = editionDefaults(edition);
        auto const face = [&](text::FontWeight weight, text::FontSlant slant) {
            return text::FontDescription { .familyName = std::string { defaults.fontFamily },
                                           .weight = weight,
                                           .slant = slant,
                                           .spacing = text::FontSpacing::Mono,
                                           .strictSpacing = false,
                                           .features = {} };
        };
        return vtrasterizer::FontDescriptions {
            .dpiScale = 1.0,
            .dpi = { 0, 0 },
            .size = { defaults.fontSize },
            .regular = face(text::FontWeight::Normal, text::FontSlant::Normal),
            .bold = face(text::FontWeight::Bold, text::FontSlant::Normal),
            .italic = face(text::FontWeight::Normal, text::FontSlant::Italic),
            .boldItalic = face(text::FontWeight::Bold, text::FontSlant::Italic),
            .emoji = text::FontDescription { .familyName = { "emoji" } },
            .renderMode = text::RenderMode::Gray,
            .textShapingEngine = vtrasterizer::TextShapingEngine::OpenShaper,
            .fontLocator = vtrasterizer::FontLocatorEngine::Native,
            .builtinBoxDrawing = true,
            .maxFallbackCount = vtrasterizer::DefaultMaxFallbackCount,
            .textOutline = {},
        };
    }

    /// The palette a built-in scheme name stands for, resolved as the loader resolves a name that no
    /// `color_schemes:` entry or side file overrides.
    /// @param name A built-in scheme name, or "default".
    /// @return The scheme's palette; the default palette for a name no built-in scheme answers to.
    [[nodiscard]] vtbackend::ColorPalette builtinPalette(std::string_view name)
    {
        auto palette = vtbackend::ColorPalette {};
        (void) vtbackend::defaultColorPalettes(name, palette);
        return palette;
    }
} // namespace

vtrasterizer::FontDescriptions const& defaultFont(DefaultsEdition edition)
{
    static auto const values = std::array {
        makeDefaultFont(DefaultsEdition::Classic),
        makeDefaultFont(DefaultsEdition::Native2026),
    };
    static_assert(std::tuple_size_v<decltype(values)> == configEnumValues<DefaultsEdition>().size(),
                  "every DefaultsEdition needs its font");
    return values[static_cast<size_t>(edition)];
}

ColorConfig defaultColors(DefaultsEdition edition)
{
    auto const& defaults = editionDefaults(edition);
    if (defaults.darkColorScheme == defaults.lightColorScheme)
        return SimpleColorConfig { .colorScheme = std::string { defaults.darkColorScheme },
                                   .colors = builtinPalette(defaults.darkColorScheme) };
    return DualColorConfig { .colorSchemeLight = std::string { defaults.lightColorScheme },
                             .colorSchemeDark = std::string { defaults.darkColorScheme },
                             .darkMode = builtinPalette(defaults.darkColorScheme),
                             .lightMode = builtinPalette(defaults.lightColorScheme) };
}

std::string defaultConfigString()
{
    Config const config {};
    auto configString = createString(config);

    return configString;
}

error_code createDefaultConfig(fs::path const& path)
{
    std::error_code ec;
    if (!path.parent_path().empty())
    {
        fs::create_directories(path.parent_path(), ec);
        if (ec)
            return ec;
    }

    ofstream { path, ios::binary | ios::trunc } << defaultConfigString();

    return error_code {};
}

std::string documentationGlobalConfig()
{
    return documentationGlobalConfig<DocumentationWriter>(Config {});
}

std::string documentationProfileConfig()
{
    return documentationProfileConfig<DocumentationWriter>(Config {});
}

std::string defaultConfigFilePath()
{
    // UTF-8, as a command line naming a configuration file is read (@see loadConfigNamed).
    return core::pathToUtf8(configHome() / "contour.yml");
}

Config loadConfig()
{
    return loadConfigFromFile(configHome() / "contour.yml");
}

namespace
{
    /// The `defaults_edition` one configuration document states.
    /// @param path The document: contour.yml or settings.yml.
    /// @return The edition it names; nothing when the file is absent, unreadable or names none. A
    ///         value no edition answers to is reported and is the same as none.
    [[nodiscard]] std::optional<DefaultsEdition> statedDefaultsEdition(fs::path const& path)
    {
        std::error_code ec;
        if (!fs::exists(path, ec) || ec)
            return std::nullopt;
        auto const document = YAMLConfigReader::parseSideFile(path);
        if (!document)
            return std::nullopt; // The full load reports a broken file; the scan only looks for its key.
        auto const node = (*document)[std::string { DefaultsEditionKey }];
        if (!node || !node.IsScalar())
            return std::nullopt;
        auto const token = node.as<std::string>();
        auto const edition = configEnumFromToken<DefaultsEdition>(token);
        if (!edition)
            errorLog()("{}: {} '{}' names no edition; ignoring it.",
                       core::pathToUtf8(path),
                       DefaultsEditionKey,
                       token);
        return edition;
    }
} // namespace

DefaultsEdition scanDefaultsEdition(fs::path const& configFile)
{
    // settings.yml is read second so that it wins: it is where the GUI records a switch to new
    // defaults, and the hand-maintained contour.yml is never rewritten for it.
    auto edition = DefaultsEdition::Classic;
    for (auto const& path: { configFile, configFile.parent_path() / "settings.yml" })
        if (auto const stated = statedDefaultsEdition(path))
            edition = *stated;
    return edition;
}

Config loadConfigFromFile(fs::path const& fileName)
{
    // Created first, so the scan always finds a file: a fresh install's is the generated one, which
    // states the newest edition, and a file that predates the key states none and so stays Classic.
    createFileIfNotExists(fileName);

    auto config = Config { .defaultsEdition = scanDefaultsEdition(fileName) };

    loadConfigFromFile(config, fileName);

    return config;
}

std::expected<Config, std::string> tryLoadConfigFromFile(fs::path const& fileName)
{
    try
    {
        return loadConfigFromFile(fileName);
    }
    catch (std::exception const& e)
    {
        return std::unexpected { std::format("cannot load config: {}", e.what()) };
    }
}

fs::path sideFileName(std::string_view name)
{
    return core::pathFromUtf8(std::format("{}.yml", name)).value_or(fs::path {});
}

std::string spelledWorkingDirectory(fs::path const& workingDirectory)
{
    if (workingDirectory.empty() || workingDirectory == homeResolvedPath("~", Process::homeDirectory()))
        return std::string { "\"~\"" };
    return core::pathToUtf8(workingDirectory);
}

namespace
{
    /// The configuration file a command line names, or the default one when it names none.
    /// @param configPath The file, UTF-8 as the command line is read; empty for the default one.
    /// @return The file to load.
    [[nodiscard]] fs::path configFileNamed(std::string_view configPath)
    {
        if (configPath.empty())
            return configHome() / "contour.yml";
        return core::pathFromUtf8(configPath).value_or(fs::path {});
    }
} // namespace

Config loadConfigNamed(std::string_view configPath)
{
    return loadConfigFromFile(configFileNamed(configPath));
}

vtbackend::Settings emulationSettings(Config const& config, TerminalProfile const& profile)
{
    auto settings = vtbackend::Settings {};
    settings.pageSize = profile.terminalSize.value();
    settings.historyLimits = profile.history.value().limits();
    auto const& folding = config.folding.value();
    // Through the one conversion the window geometry reserves the strip's width from.
    settings.gutter = gutterSettingsFor(config);
    settings.autoCollapseFoldOnNewCommand = folding.autoCollapseOnNewCommand;
    settings.foldJumpBehavior = folding.onJumpIntoFold;
    settings.foldingAvailability = folding.availability();
    settings.commandBlockLimits =
        vtbackend::CommandBlockStoreLimits { .maxRecords = config.commandBlocks.value().maxRecords };

    // `enabled` travels as itself rather than being conjoined into the two policies below: the engine
    // honours it where the sequence arrives, which is what makes it gate the ancestry -- and with it
    // the breadcrumb and the daemon's replication -- and not merely the two knobs somebody remembered.
    auto const& oscContext = config.oscContext.value();
    settings.contextSignalling =
        oscContext.enabled ? vtbackend::ContextSignalling::Enabled : vtbackend::ContextSignalling::Disabled;
    settings.contextLimits = vtbackend::ContextStackLimits { .maxDepth = oscContext.maxDepth,
                                                             .maxRetained = oscContext.maxRetained };
    settings.contextMarkPolicy = oscContext.deriveMarkers;
    settings.contextTintScope = oscContext.tinting;
    settings.terminalId = profile.terminalId.value();
    settings.frozenModes = profile.frozenModes.value();
    settings.maxImageRegisterCount = config.images.value().maxImageColorRegisters;
    settings.goodImageProtocol = config.images.value().goodImageProtocol;
    settings.sixelScrolling = config.images.value().sixelScrolling ? vtbackend::SixelScrolling::Enabled
                                                                   : vtbackend::SixelScrolling::Disabled;
    settings.primaryScreen.allowReflowOnResize = config.reflowOnResize.value();
    settings.graphemeClustering = config.graphemeClustering.value();
    settings.wordDelimiters = unicode::from_utf8(config.wordDelimiters.value());
    return settings;
}

vtbackend::GutterSettings gutterSettingsFor(Config const& config)
{
    auto const& gutter = config.gutter.value();
    return vtbackend::GutterSettings {
        .foldMarkers = config.folding.value().markersVisible(),
        .exitStatus = gutter.exitStatus,
        .userMarks = gutter.userMarks,
        .blockColumnWidth = gutter.blockColumnWidth,
        .lineNumbers = gutter.lineNumbers,
        .lineNumberWidth = gutter.lineNumberWidth,
        .timestamps = gutter.timestamps,
        .timestampFormat = gutter.timestampFormat,
        .hoverStroke =
            gutter.boldOnHover ? vtbackend::FoldHoverStroke::Bolder : vtbackend::FoldHoverStroke::Unchanged,
    };
}

namespace
{
    /// A profile, with the name it was found under.
    struct NamedProfile
    {
        std::string name; ///< The name actually resolved (the config's default, if none was asked for).
        TerminalProfile const* profile = nullptr;
    };

    /// The resolve-refuse half every profile lookup by a user-given name shares: @p profileName within
    /// @p config, or the configuration's default one when @p profileName is empty.
    ///
    /// One place, so a daemon, the client that attaches to it and the client's own new sessions cannot
    /// come to refuse a profile differently.
    /// @return The profile, or a human-readable reason it could not be resolved. An unknown profile is a
    ///         failure rather than a fall back to the default one: hosting sessions under a profile the
    ///         user did not name is a misconfiguration they would discover only much later, through the
    ///         wrong scrollback depth or the wrong reported terminal.
    [[nodiscard]] std::expected<NamedProfile, std::string> findNamedProfile(Config const& config,
                                                                            std::string const& profileName)
    {
        auto name = profileName.empty() ? config.defaultProfileName.value() : profileName;
        auto const* const profile = config.findProfile(name);
        if (profile == nullptr)
            return std::unexpected { std::format(
                "unknown profile '{}' in '{}'", name, core::pathToUtf8(config.configFile)) };
        return NamedProfile { .name = std::move(name), .profile = profile };
    }

} // namespace

std::expected<Config, std::string> tryLoadConfigNamed(std::string_view configPath)
{
    return tryLoadConfigFromFile(configFileNamed(configPath));
}

std::expected<vtbackend::Settings, std::string> emulationSettingsFor(Config const& config,
                                                                     std::string const& profileName)
{
    return findNamedProfile(config, profileName).transform([&](NamedProfile const& found) {
        return emulationSettings(config, *found.profile);
    });
}

std::expected<HostEntry, std::string> resolveHost(std::string const& configPath, std::string_view nameOrUrl)
{
    if (nameOrUrl.contains("://"))
        return parseHostUrl(nameOrUrl)
            .transform_error(
                [&](HostUrlError error) { return std::format("{}: {}", nameOrUrl, describe(error)); })
            .and_then([](HostUrl url) -> std::expected<HostEntry, std::string> {
                // A TCP daemon always requires a token, and only a saved host can name its file.
                if (url.scheme == HostScheme::Tls)
                    return std::unexpected { std::string {
                        "a tls:// host needs a token; save it under hosts: with token_file" } };
                return HostEntry {
                    .url = std::move(url), .tokenFile = {}, .tlsCa = {}, .remoteCommand = {}, .profile = {}
                };
            });

    return tryLoadConfigFromFile(configFileNamed(configPath))
        .and_then([&](Config const& loaded) -> std::expected<HostEntry, std::string> {
            auto const& hosts = loaded.hosts.value();
            if (auto const found = hosts.find(std::string { nameOrUrl }); found != hosts.end())
                return found->second;
            auto const names = std::ranges::to<std::vector<std::string>>(std::views::keys(hosts));
            return std::unexpected { std::format("no host named '{}' in {} (saved hosts: {})",
                                                 nameOrUrl,
                                                 core::pathToUtf8(loaded.configFile),
                                                 names.empty() ? std::string { "(none)" }
                                                               : core::joinHumanReadable(names, ", ")) };
        });
}

vtbackend::ColorPalette const* preferredColorPalette(ColorConfig const& colorConfig,
                                                     vtbackend::ColorPreference preference)
{
    if (auto const* dualColorConfig = std::get_if<DualColorConfig>(&colorConfig))
    {
        switch (preference)
        {
            case vtbackend::ColorPreference::Dark: return &dualColorConfig->darkMode;
            case vtbackend::ColorPreference::Light: return &dualColorConfig->lightMode;
        }
    }
    else if (auto const* simpleColorConfig = std::get_if<SimpleColorConfig>(&colorConfig))
        return &simpleColorConfig->colors;

    errorLog()("preferredColorPalette: Unknown color config type.");
    return nullptr;
}

std::optional<std::string> skippedHintPatternReason(HintPatternConfig const& pattern)
{
    if (pattern.name.empty())
        return "a pattern needs a name";
    if (pattern.regex.empty() && !vtbackend::isBuiltinLinkPatternName(pattern.name))
        return std::format("pattern '{}' needs a regex, as it names no built-in pattern ({})",
                           pattern.name,
                           core::joinHumanReadable(vtbackend::builtinLinkPatternNames()));
    return std::nullopt;
}

std::vector<vtbackend::LinkPatternSpec> linkPatternSpecs(TerminalProfile const& profile)
{
    auto specs = std::vector<vtbackend::LinkPatternSpec> {};
    auto const activationOf = [](bool clickable) {
        return clickable ? vtbackend::LinkActivation::Clickable : vtbackend::LinkActivation::HintOnly;
    };
    for (auto const& pattern: profile.hintPatterns.value())
        specs.push_back(vtbackend::LinkPatternSpec {
            .name = pattern.name,
            .regex = pattern.regex.empty() ? std::nullopt : std::optional { pattern.regex },
            .activation = pattern.clickable.transform(activationOf),
            .urlTemplate = pattern.url,
        });
    return specs;
}

std::shared_ptr<vtbackend::LinkPatterns const> linkPatternsFor(TerminalProfile const& profile)
{
    if (profile.hintPatterns.value().empty())
        return vtbackend::LinkPatterns::builtins();
    return std::make_shared<vtbackend::LinkPatterns const>(
        vtbackend::LinkPatterns::compile(linkPatternSpecs(profile)));
}

std::optional<vtbackend::Modifiers> followHyperlinkModifiers(Config const& config)
{
    for (auto const& mapping: config.inputMappings.value().mouseMappings)
        if (mapping.input == vtbackend::MouseButton::Left
            && containsAction<actions::FollowHyperlink>(mapping.binding))
            return mapping.modifiers;
    return std::nullopt;
}

vtbackend::Settings sessionSettings(Config const& config,
                                    TerminalProfile const& profile,
                                    vtbackend::ColorPreference colorPreference,
                                    std::optional<vtbackend::PageSize> initialPageSize)
{
    // The emulation half comes from the shared table, so a GUI-hosted session and a
    // daemon-hosted one can never disagree on what the terminal IS; only the presentation
    // fields below decide how it is PRESENTED, which used to be applied for a local session
    // only (see TerminalSession::createSettingsFromConfig, which this replaces).
    auto settings = emulationSettings(config, profile);

    // A new tab/split inherits the live window's running grid; a brand-new window keeps the
    // profile's configured terminalSize, which the shared table already applied. Overridden here
    // so the terminal is BORN at the right size, not just corrected once a display attaches
    // (which never happens for a background tab).
    if (initialPageSize)
        settings.pageSize = *initialPageSize;

    // Focus is granted, never assumed: a session born focused (a background tab, a non-active
    // split pane, a daemon-hosted session before any client attaches) would otherwise render an
    // active cursor and withhold the DECSET 1004 focus-out its application is due.
    settings.focused = false;
    // The session's read loop writes what the GUI thread types and pastes, so that the GUI thread never parks
    // on a write a program leaves unread.
    settings.inputWriter = vtbackend::InputWriter::ReaderThread;
    settings.ptyBufferObjectSize = config.ptyBufferObjectSize.value();
    settings.ptyReadBufferSize = config.ptyReadBufferSize.value();
    settings.mouseWheelScrollMultiplier = profile.history.value().historyScrollMultiplier;
    settings.autoScrollOnUpdate = profile.history.value().autoScrollOnUpdate;
    settings.copyLastMarkRangeOffset = profile.copyLastMarkRangeOffset.value();
    settings.cursorBlinkInterval = profile.modeInsert.value().cursor.cursorBlinkInterval;
    settings.cursorShape = profile.modeInsert.value().cursor.cursorShape;
    settings.cursorDisplay = profile.modeInsert.value().cursor.cursorDisplay;
    settings.blinkStyle = profile.blinkStyle.value();
    settings.screenTransitionStyle = profile.screenTransitionStyle.value();
    settings.screenTransitionDuration = profile.screenTransitionDuration.value();
    settings.cursorMotionAnimationDuration = profile.cursorMotionAnimationDuration.value();
    settings.smoothLineScrolling = profile.smoothLineScrolling.value();
    settings.smoothScrolling = profile.smoothScrolling.value();
    settings.momentumScrolling = profile.momentumScrolling.value();
    settings.mouseProtocolBypassModifiers = config.bypassMouseProtocolModifiers.value();
    settings.linkPatterns = linkPatternsFor(profile);
    settings.linkDetection = linkDetectionPolicyOf(profile.linkDetection.value());
    settings.linkDecorationMode = profile.hyperlinkDecoration.value().mode;
    settings.linkFollowModifiers = followHyperlinkModifiers(config);
    settings.statusDisplayType = profile.statusLine.value().initialType;
    settings.statusDisplayPosition = profile.statusLine.value().position;
    settings.indicatorStatusLine.left = profile.statusLine.value().indicator.left;
    settings.indicatorStatusLine.middle = profile.statusLine.value().indicator.middle;
    settings.indicatorStatusLine.right = profile.statusLine.value().indicator.right;
    settings.tabNamingMode = [&]() {
        // Find the Tabs segment among the status line's three (left/middle/right): whichever one
        // references it names how tabs are indexed.
        auto const& indicator = profile.statusLine.value().indicator;
        auto const segments = std::array { &indicator.left, &indicator.middle, &indicator.right };
        auto const it = std::ranges::find_if(segments, [](auto const* s) { return s->contains("Tabs"); });
        if (it == segments.end())
            return vtbackend::TabsNamingMode::Indexing;
        std::string_view const segment = **it;

        // check if indexing is defined
        auto constexpr IndexingKey = std::string_view { "Indexing=" };
        if (auto const key = segment.find(IndexingKey); key != std::string_view::npos)
        {
            // cut the string after indexing=, then cut its right part off at the next delimiter
            auto value = segment.substr(key + IndexingKey.size());
            value = value.substr(0, std::min(value.find(','), value.find('}')));

            if (core::toLower(value) == "title")
                return vtbackend::TabsNamingMode::Title;
        }
        return vtbackend::TabsNamingMode::Indexing;
    }();

    settings.syncWindowTitleWithHostWritableStatusDisplay =
        profile.statusLine.value().syncWindowTitleWithHostWritableStatusDisplay;
    if (auto const* p = preferredColorPalette(profile.colors.value(), colorPreference))
        settings.colorPalette = *p;
    settings.visualizeSelectedWord = profile.highlightDoubleClickedWord.value();
    settings.isInsertAfterYank = profile.insertAfterYank.value();
    settings.highlightTimeout = profile.highlightTimeout.value();
    settings.stickyHeader = profile.stickyHeader.value().settings();

    return settings;
}

ResolvedSessionConfig resolvedSessionConfig(Config const& config,
                                            TerminalProfile const& profile,
                                            std::string const& profileName,
                                            vtbackend::ColorPreference colorPreference,
                                            std::optional<vtbackend::PageSize> initialPageSize)
{
    auto settings = sessionSettings(config, profile, colorPreference, initialPageSize);

    // The shell: a copy of the profile's configured ExecInfo (program/arguments/workingDirectory
    // already carry the profile's `shell:` block), with CONTOUR_PROFILE added exactly as a local
    // session's does (see ContourGuiApp::terminalGuiAction) so a hosted shell can tell which
    // profile it is running under just like a local one can.
    auto shell = profile.shell.value();
    shell.env["CONTOUR_PROFILE"] = profileName;

    // An unset `initial_working_directory` resolves to empty (YAMLConfigReader::loadFromEntry
    // only substitutes `~`, not "absent"), which vtpty::Process then leaves as "inherit the
    // hosting process's cwd" -- fine for a locally-launched GUI inheriting the user's shell cwd,
    // wrong for a daemon whose own cwd is whatever started it (e.g. systemd's WorkingDirectory=).
    // Falling back to the home directory here matches what every hosted shell used to get
    // unconditionally (see the old ContourApp::daemonAction, which set
    // config.shell.workingDirectory = Process::homeDirectory()) and is exactly what an
    // interactive shell's own "no explicit cwd" default is.
    if (shell.workingDirectory.empty())
        shell.workingDirectory = Process::homeDirectory();

    // The profile's configured startup layout: looked up by name in `layouts:`, exactly like
    // TerminalSessionManager::findLayout does for the local-GUI path. An unset or unknown name
    // degrades to "no startup layout" rather than failing resolution — a typo'd default_layout
    // must not keep the daemon from starting at all.
    auto startupLayout = Layout {};
    if (auto const& layoutName = config.defaultLayoutName.value(); !layoutName.empty())
    {
        auto const& layouts = config.layouts.value();
        if (auto const it = layouts.find(layoutName); it != layouts.end())
            startupLayout = it->second;
        else
            configLog()("resolveSessionConfig: no layout named '{}' (default_layout); "
                        "the daemon will start with a single default tab.",
                        layoutName);
    }

    return ResolvedSessionConfig {
        .settings = std::move(settings),
        .shell = std::move(shell),
        .escapeSandbox = profile.escapeSandbox.value(),
        .startupLayout = std::move(startupLayout),
        .configFile = config.configFile,
        .extensions = profile.extensions.value(),
        .statusLineExtension = profile.statusLine.value().extension,
    };
}

std::expected<ResolvedSessionConfig, std::string> resolvedSessionConfigFor(Config const& config,
                                                                           std::string const& profileName)
{
    return findNamedProfile(config, profileName).transform([&](NamedProfile const& found) {
        // No dark/light preference to honor headlessly: a daemon presents nothing itself, so its
        // hosted sessions get the color config's default (light) half until an attaching client's
        // presentation states its own preference over the wire, the same way it states any other
        // per-client preference.
        return resolvedSessionConfig(config, *found.profile, found.name, vtbackend::ColorPreference::Light);
    });
}

std::expected<ResolvedSessionConfig, std::string> resolveSessionConfig(std::string const& configPath,
                                                                       std::string const& profileName)
{
    return tryLoadConfigNamed(configPath).and_then([&](Config const& loaded) {
        return resolvedSessionConfigFor(loaded, profileName);
    });
}

void compareEntries(Config& config, auto const& output)
{
    // compare entries in config and default config and log differences
    auto defaultConfig = YAML::Load(defaultConfigString());
    auto userConfig = YAML::Load(readFile(config.configFile).value_or(std::string {}));
    // A file that predates the key is the Classic edition by its absence, not a file missing a setting.
    defaultConfig.remove(std::string { DefaultsEditionKey });

    std::vector<std::string> existingEntries;
    std::vector<std::string> userEntries;
    auto collectEntries = [&](auto self, YAML::Node const& node, auto& where, std::string const& root) {
        if (node.IsScalar())
        {
            // delete . in the end of the string
            where.push_back(root.substr(0, root.size() - 1));
            return;
        }
        if (node.IsMap())
            for (auto const& entry: node)
                self(self, entry.second, where, root + entry.first.as<std::string>() + ".");
    };

    collectEntries(collectEntries, defaultConfig, existingEntries, "");
    collectEntries(collectEntries, userConfig, userEntries, "");

    for (auto const& entry: existingEntries)
    {
        if (std::ranges::find(userEntries, entry) == userEntries.end())
        {
            output()("[diff] Entry {} is missing in user config file\n", entry);
        }
    }
}

/// Records the provenance of every profile and color scheme the main parse produced, so that anything
/// the GUI side-file scan adds on top is distinguishable as GUI-owned (and therefore editable in the
/// settings page) from the hand-maintained inline entries, which stay read-only there.
/// @param config The just-parsed configuration to annotate.
/// @param reader The reader that parsed contour.yml; its @c doc supplies the inline `color_schemes:` node.
static void recordMainConfigOrigins(Config& config, YAMLConfigReader const& reader)
{
    for (auto const& [name, _]: config.profiles.value())
        config.profileOrigins.emplace(name, SettingsOrigin::MainConfig);
    for (auto const& [name, _]: config.colorschemes.value())
        config.colorSchemeOrigins.emplace(
            name, name == "default" ? SettingsOrigin::Builtin : SettingsOrigin::MainConfig);

    // Inline color schemes are resolved lazily INTO the referencing profile, not into the colorschemes
    // map, so their names never reach the loop above. Record them straight from the contour.yml
    // `color_schemes:` node: without this the settings page cannot tell that a GUI scheme name collides
    // with a hand-maintained inline scheme (which shadows a side file at load), and would let the user
    // write a dead colorschemes/<name>.yml that never takes effect.
    if (auto const node = reader.doc["color_schemes"]; node && node.IsMap())
        for (auto const& entry: node)
        {
            auto const name = entry.first.as<std::string>();
            config.colorSchemeOrigins.emplace(
                name, name == "default" ? SettingsOrigin::Builtin : SettingsOrigin::MainConfig);
        }
}

/// settings.yml's document. Nothing saved yet is no error: that is an empty document.
/// @param path The `settings.yml` path, beside contour.yml.
/// @return The document, or why there is none.
[[nodiscard]] static std::expected<YAML::Node, std::string> parseGuiSettingsFile(fs::path const& path)
{
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return YAML::Node {};
    return YAMLConfigReader::parseSideFile(path);
}

/// The GUI-owned settings a parsed settings.yml holds.
/// @param document The parsed settings.yml.
/// @return Its default profile and its global overrides.
[[nodiscard]] static GuiManagedSettings guiSettingsFrom(YAML::Node const& document)
{
    auto settings = GuiManagedSettings {};
    if (auto const node = document["default_profile"]; node && node.IsScalar())
        settings.defaultProfile = node.as<std::string>();

    // Every other top-level scalar is a GUI global override, kept as its YAML scalar text so it can be
    // re-emitted verbatim and re-applied through the typed per-key loader on the next load.
    if (document.IsMap())
        for (auto const& entry: document)
        {
            if (!entry.first.IsScalar())
                continue; // a complex YAML key cannot name a setting
            auto const key = entry.first.as<std::string>();
            if (key == "default_profile")
                continue;
            if (entry.second.IsScalar())
                settings.globalOverrides[key] = entry.second.as<std::string>();
            else if (entry.second.IsMap())
                for (auto const& child: entry.second)
                    if (child.first.IsScalar() && child.second.IsScalar())
                        settings.globalOverrides[key + '.' + child.first.as<std::string>()] =
                            child.second.as<std::string>();
        }

    return settings;
}

/// Merges the GUI-managed side files over the just-parsed configuration, mirroring the layouts.yml
/// merge: the freshest (GUI) content wins, and a broken file is logged and skipped rather than taking
/// the whole configuration down. Color scheme side files (colorschemes/<name>.yml) are already
/// resolved lazily by the reader when a profile references them, so only the per-profile files and the
/// global settings.yml are handled here.
/// @param config The configuration to augment in place (its @c configFile locates the side files).
/// @param reader The reader used for the main config; reused to parse profile bodies (it carries the
///               logger and ${VAR} replacer, and loadProfileBody takes its node explicitly).
static void mergeGuiManagedSideFiles(Config& config, YAMLConfigReader& reader)
{
    auto const dir = config.configFile.parent_path();

    // Inheritance base for GUI profiles: the resolved default profile, captured BEFORE any side file
    // can replace it in the map, so a GUI profile inherits the same defaults an inline one would.
    auto base = TerminalProfile { .defaultsEdition = config.defaultsEdition };
    if (auto const* def = config.findProfile(config.defaultProfileName.value()))
        base = *def;

    auto const profilesDir = dir / "profiles";
    std::error_code ec;
    if (std::filesystem::is_directory(profilesDir, ec))
    {
        auto files = std::vector<std::filesystem::path> {};
        for (auto const& entry: std::filesystem::directory_iterator(profilesDir, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".yml")
                files.push_back(entry.path());
        std::ranges::sort(files); // deterministic load order across filesystems

        for (auto const& path: files)
        {
            auto const name = core::pathToUtf8(path.stem());
            auto const root = YAMLConfigReader::parseSideFile(path);
            if (!root)
            {
                configLog()("Skipping the profile side file {}", root.error());
                continue;
            }
            auto profile = base; // inherit the default profile's values, then overlay the file's keys
            // The file's root is the profile's body, so a malformed entry is reported as `<file>: <key>`;
            // the key alone would name neither the file nor the profile.
            reader.readSideFile(path, *root, [&]() { reader.loadProfileBody(*root, profile, name); });
            config.profiles.value()[name] = std::move(profile);
            config.profileOrigins[name] = SettingsOrigin::SideFile;
        }
    }

    // settings.yml — GUI-owned globals. Applied last so a default_profile can name a profile the scan
    // above just introduced. Read and parsed once: the overrides applied are the ones listed, even when
    // the settings page saves the file in between.
    auto const settingsPath = dir / "settings.yml";
    auto const settingsDocument = parseGuiSettingsFile(settingsPath);
    if (auto loaded = settingsDocument.transform(guiSettingsFrom))
    {
        config.guiManagedSettings = *loaded;
        if (auto const& defaultProfile = loaded->defaultProfile)
        {
            if (config.findProfile(*defaultProfile))
                config.defaultProfileName = *defaultProfile;
            else
                configLog()("settings.yml default_profile '{}' is not a known profile; keeping '{}'.",
                            *defaultProfile,
                            config.defaultProfileName.value());
        }

        // Apply any GUI global overrides present in settings.yml through the SAME typed per-key loaders
        // contour.yml uses (an absent key is a no-op), so the GUI never needs its own parse logic and a
        // new overridable global is just another row of globalOverrides().
        if (!loaded->globalOverrides.empty())
        {
            auto overrides =
                YAMLConfigReader(settingsPath, *settingsDocument, configLog, configEnvironment());
            // Nested sections: the section loader touches only the keys settings.yml actually has, so an
            // override of gutter.line_numbers leaves a gutter.timestamps from contour.yml as it was. It runs
            // only when settings.yml overrides a leaf of the section, so a hand-edited scalar `renderer: foo`
            // is ignored instead of being subscripted.
            // The section loaders expect the shapes contour.yml documents; settings.yml is hand-editable,
            // and a malformed entry there must cost that leaf, never startup. loadFromEntry() reads a
            // ConfigEntry under loadGuarded(), which puts it back as it was and logs it, and a section's
            // loader reads each of its leaves under a guard of its own the same way.
            // The keys and their loaders are the rows of globalOverrides(). A scalar's runs whether or not
            // settings.yml has its key: the loader skips an absent one and reports one of the wrong shape.
            // Read as the side file it is: settings.yml spells its keys as contour.yml does, so a malformed
            // entry is reported as `<settings.yml>: <key>`, not as if contour.yml held it.
            overrides.readSideFile(settingsPath, overrides.doc, [&]() {
                for (auto const& row: globalOverrides())
                    if (row.shape == GlobalOverrideShape::Scalar
                        || std::ranges::any_of(loaded->globalOverrides | std::views::keys,
                                               [&row](std::string const& key) { return row.covers(key); }))
                        row.apply(overrides, config, std::string { row.key });
            });
        }
    }
    else
        configLog()("Failed to load settings.yml: {}", loaded.error());
}

/**
 * @return success or failure of loading the config file.
 */
void loadConfigFromFile(Config& config, fs::path const& fileName)
{
    auto logger = configLog;
    logger()("Loading configuration from file: {} ", core::pathToUtf8(fileName));
    config.configFile = fileName;
    createFileIfNotExists(config.configFile);

    // A caller may reload into a Config object it already loaded rather than a fresh one, so the
    // collections rebuilt wholesale from the file plus the GUI side-file scan below must be reset to their
    // default-constructed starting state first. Otherwise a profile or color scheme that was deleted from
    // disk (e.g. a GUI entity removed in the settings page) would linger from the previous load, and its
    // stale provenance would keep the settings page treating a contour.yml entry as GUI-owned. The loaders
    // assume the built-in seeds exist (the "main" profile is the inheritance base; the "default" color
    // palette is the fallback), so restore those — exactly a first load's initial Config state.
    config.profiles.value() = { { "main", TerminalProfile { .defaultsEdition = config.defaultsEdition } } };
    config.colorschemes.value() = { { "default", vtbackend::ColorPalette {} } };
    config.profileOrigins.clear();
    config.colorSchemeOrigins.clear();

    auto yamlVisitor = YAMLConfigReader(config.configFile, logger, configEnvironment());
    yamlVisitor.load(config);

    // Merge the machine-managed sibling layouts.yml (written by SaveLayout). Its entries override
    // same-named inline layouts, since the file is the freshest SaveLayout output. A file that
    // fails to parse is reported and skipped: a broken layouts.yml must not take the rest of the
    // configuration down with it.
    if (auto fileLayouts = loadLayoutsFile(config.configFile.parent_path() / "layouts.yml"))
    {
        for (auto& [name, layout]: *fileLayouts)
            config.layouts.value()[name] = std::move(layout);
    }
    else
        logger()("Failed to load layouts.yml: {}", fileLayouts.error());

    // Provenance first, then the GUI side files (profiles/*.yml, settings.yml) on top — the same
    // freshest-wins, broken-file-tolerant merge philosophy the layouts.yml merge above uses.
    recordMainConfigOrigins(config, yamlVisitor);
    mergeGuiManagedSideFiles(config, yamlVisitor);

    compareEntries(config, logger);
}

std::expected<std::unordered_map<std::string, Layout>, std::string> loadLayoutsFile(fs::path const& path)
{
    std::unordered_map<std::string, Layout> layouts;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return layouts; // nothing saved yet is not an error

    // Surface YAML parse failures to the caller: YAMLConfigReader's constructor swallows them
    // (by design, for the main config's "load defaults instead" path), but SaveLayout must NOT
    // mistake an unreadable file for an empty one — rewriting it would destroy every layout it
    // failed to read.
    auto document = YAML::Node {};
    try
    {
        auto const contents = readFile(path);
        if (!contents)
            return std::unexpected(std::format("cannot read {}", core::pathToUtf8(path)));
        document = YAML::Load(*contents);
    }
    catch (std::exception const& e)
    {
        return std::unexpected(std::string(e.what()));
    }

    auto reader = YAMLConfigReader(path, document, configLog, configEnvironment());
    reader.loadLayoutsInto(layouts);
    return layouts;
}

optional<std::string> readConfigFile(std::string const& filename)
{
    for (fs::path const& prefix: configHomes("contour"))
        if (auto text = readFile(prefix / filename); text.has_value())
            return text;

    return nullopt;
}

namespace
{
    /// @return The document in @p file; an empty one, the reason logged, where it cannot be read or parsed.
    [[nodiscard]] YAML::Node documentOrNone(fs::path const& file)
    {
        try
        {
            // Read through readFile (binary + CRLF->LF normalization) rather than YAML::LoadFile, so every
            // config document — contour.yml, the settings.yml global-override reader, and the color-scheme
            // files — is parsed identically regardless of how it was saved. YAML::LoadFile is CRLF-safe
            // only on Windows (its text-mode ifstream strips '\r' at the OS layer); a CRLF file synced onto
            // Linux/macOS would otherwise leave a trailing '\r' on plain scalar values and fail typed
            // conversion. This is the same normalization the GUI profile side files already rely on.
            if (auto const contents = readFile(file))
                return YAML::Load(*contents);
            errorLog()("Configuration file could not be read.\nDefault config will be loaded.");
        }
        catch (std::exception const& e)
        {
            errorLog()("Configuration file is corrupted. {}\nDefault config will be loaded.", e.what());
        }
        return {};
    }
} // namespace

YAMLConfigReader::YAMLConfigReader(fs::path const& filename,
                                   core::log::Category const& log,
                                   core::Environment const& env,
                                   VariableReplacer replacer):
    YAMLConfigReader(filename, documentOrNone(filename), log, env, std::move(replacer))
{
}

YAMLConfigReader::YAMLConfigReader(fs::path filename,
                                   YAML::Node const& document,
                                   core::log::Category const& log,
                                   core::Environment const& env,
                                   VariableReplacer replacer):
    configFile(std::move(filename)),
    doc { document },
    logger { log },
    variableReplacer { std::move(replacer) }
{
    if (!variableReplacer)
    {
        variableReplacer = [&log = logger, &env](std::string_view name) -> std::string {
            if (auto value = env.get(name))
                return std::move(*value);
            log()("Undefined environment variable: ${{{}}}", name);
            return {};
        };
    }
}

std::string YAMLConfigReader::resolveVariables(std::string const& input) const
{
    return replaceVariables(input, variableReplacer);
}

std::filesystem::path YAMLConfigReader::declaringDirectory() const
{
    auto file = configFile;
    // Inside a side file the file is the one being read, not the configuration file it belongs to.
    if (!_keyPath.empty() && _keyPath.front().kind == KeyStepKind::File)
        if (auto sideFile = core::pathFromUtf8(_keyPath.front().entry))
            file = *std::move(sideFile);
    // The file first, then its parent: a bare file name has an empty parent, which has no absolute form.
    auto error = std::error_code {};
    auto absolute = fs::absolute(file, error);
    return error ? file.parent_path() : absolute.parent_path();
}

std::filesystem::path YAMLConfigReader::resolvedPath(std::string const& input) const
{
    auto const text = resolveVariables(input);
    auto path = homeResolvedPath(text, vtpty::Process::homeDirectory());
    if (!path)
    {
        errorLog()("Ignoring the path '{}': {}", core::escape(text), path.error().message());
        return {};
    }
    return std::move(*path);
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::filesystem::path& where) const
{
    auto const child = node[entry];
    if (child)
    {
        where = resolvedPath(child.as<std::string>());
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     RenderingBackend& where)
{
    auto const child = node[entry];
    if (child)
    {
        auto const rawValue = child.as<std::string>();
        auto const renderBackendStr = core::toUpper(rawValue);
        if (renderBackendStr == "AUTO" || renderBackendStr == "DEFAULT")
            where = RenderingBackend::Auto;
        else if (renderBackendStr == "OPENGL")
            where = RenderingBackend::OpenGL;
        else if (renderBackendStr == "VULKAN")
            where = RenderingBackend::Vulkan;
        else if (renderBackendStr == "DIRECT3D11" || renderBackendStr == "D3D11")
            where = RenderingBackend::Direct3D11;
        else if (renderBackendStr == "DIRECT3D12" || renderBackendStr == "D3D12")
            where = RenderingBackend::Direct3D12;
        else if (renderBackendStr == "METAL")
            where = RenderingBackend::Metal;
        else if (renderBackendStr == "SOFTWARE")
            where = RenderingBackend::Software;
        else
            errorLog()("Unknown renderer backend '{}'; falling back to {}.", rawValue, where);

        logger()("Loading entry: {}, value {}", entry, where);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, GpuSelector& where)
{
    auto const child = node[entry];
    if (!child)
        return;
    if (!child.IsScalar())
    {
        where = GpuSelector {};
        errorLog()("Invalid renderer.gpu value (not a single value); using {}.", where);
        return;
    }
    auto const rawValue = child.as<std::string>();
    if (auto const parsed = parseGpuSelector(rawValue))
        where = *parsed;
    else
    {
        where = GpuSelector {};
        errorLog()("Invalid renderer.gpu value '{}'; using {}.", rawValue, where);
    }
    logger()("Loading entry: {}, value {}", entry, where);
}

void YAMLConfigReader::load(Config& c)
{
    try
    {

        loadFromEntry("platform_plugin", c.platformPlugin);
        if (c.platformPlugin.value() == "auto")
        {
            c.platformPlugin = "";
        }
        loadFromEntry("default_profile", c.defaultProfileName);
        loadFromEntry("default_layout", c.defaultLayoutName);
        loadFromEntry("renderer", c.renderer);
        loadFromEntry("word_delimiters", c.wordDelimiters);
        loadFromEntry("extended_word_delimiters", c.extendedWordDelimiters);
        loadFromEntry("command_palette_recent_count", c.commandPaletteRecentCount);
        loadFromEntry("read_buffer_size", c.ptyReadBufferSize);
        loadFromEntry("pty_buffer_size", c.ptyBufferObjectSize);
        loadFromEntry("images", c.images);
        loadFromEntry("daemon", c.daemon);
        loadFromEntry("folding", c.folding);
        loadFromEntry("gutter", c.gutter);
        loadFromEntry("osc_context", c.oscContext);
        loadFromEntry("command_blocks", c.commandBlocks);
        loadFromEntry("live_config", c.live);
        loadFromEntry("early_exit_threshold", c.earlyExitThreshold);
        loadFromEntry("spawn_new_process", c.spawnNewProcess);
        loadFromEntry("reflow_on_resize", c.reflowOnResize);
        loadFromEntry("snap_to_rows", c.snapToRows);
        loadFromEntry("remember_window_position", c.rememberWindowPosition);
        loadFromEntry("tab_switch_on_horizontal_wheel", c.tabSwitchOnHorizontalWheel);
        loadFromEntry("accessibility_announcements", c.accessibilityAnnouncements);
        loadFromEntry("accessibility_caret_reporting", c.accessibilityCaretReporting);
        loadFromEntry("hyperlink_hover_tooltip", c.hyperlinkHoverTooltip);
        loadFromEntry("progress_timeout", c.progressTimeout);
        loadFromEntry("notification_close_timeout", c.notificationCloseTimeout);
        loadFromEntry("tab_bar_position", c.tabBarPosition);
        loadFromEntry("tab_bar_visibility", c.tabBarVisibility);
        loadFromEntry("text_scaling_method", c.textScalingMethod);
        loadFromEntry("grapheme_clustering", c.graphemeClustering);
        loadFromEntry("gui_config_locked", c.guiConfigLocked);
        loadFromEntry("theme", c.theme);
        loadFromEntry("ui_style", c.uiStyle);
        loadFromEntry("window_control_style", c.windowControlStyle);
        loadFromEntry("ui_font_family", c.uiFontFamily);
        loadFromEntry("ui_font_size", c.uiFontSize);
        loadFromEntry("experimental", c.experimentalFeatures);
        loadFromEntry("bypass_mouse_protocol_modifier", c.bypassMouseProtocolModifiers);
        loadFromEntry("on_mouse_select", c.onMouseSelection);
        loadFromEntry("mouse_block_selection_modifier", c.mouseBlockSelectionModifiers);
        loadFromEntry("profiles", c.profiles, c.defaultProfileName.value(), c.defaultsEdition);
        loadFromEntry("layouts", c.layouts);
        loadFromEntry("hosts", c.hosts);
        loadFromEntry("git_drawings", c.gitDrawings);
        vtrasterizer::BoxDrawingRenderer::setGitDrawingsStyle(c.gitDrawings.value());
        loadFromEntry("box_arc_style", c.boxArcStyle);
        vtrasterizer::BoxDrawingRenderer::setArcStyle(c.boxArcStyle.value());
        loadFromEntry("braille_style", c.brailleStyle);
        vtrasterizer::BoxDrawingRenderer::setBrailleStyle(c.brailleStyle.value());

        // loadFromEntry("color_schemes", c.colorschemes); // NB: This is always loaded lazily
        loadFromEntry("input_mapping", c.inputMappings);

        // Said once, at the end: the mouse bindings are global and the modes are the profiles', so one
        // missing binding is one mistake however many profiles (inheriting ones included) ask for the reveal.
        if (!followHyperlinkModifiers(c))
        {
            auto revealing = std::vector<std::string> {};
            for (auto const& [name, profile]: c.profiles.value())
                if (profile.hyperlinkDecoration.value().mode == vtbackend::LinkDecorationMode::Modifier)
                    revealing.push_back(name);
            std::ranges::sort(revealing);
            if (!revealing.empty())
                errorLog()("hyperlink_decoration.mode is modifier in {} {} but no FollowHyperlink mouse "
                           "binding exists; links will never be revealed.",
                           revealing.size() == 1 ? "profile" : "profiles",
                           core::joinHumanReadable(revealing, ", "));
        }
    }
    catch (std::exception const& e)
    {
        errorLog()("Something went wrong during config file loading, check `contour debug config` output "
                   "for more info");
    }
}

std::expected<YAML::Node, std::string> YAMLConfigReader::parseSideFile(fs::path const& file)
{
    // Through readFile, which reads in binary and drops each CR, so a file saved with Windows line endings
    // parses as it does everywhere else: YAML::LoadFile would keep the CR on every scalar off Windows.
    auto const contents = readFile(file);
    if (!contents)
        return std::unexpected(std::format("{}: the file cannot be read", core::pathToUtf8(file)));

    try
    {
        // A side file holds a map of settings, or nothing yet. A bare word or a list parses too, but names
        // no setting, and subscripting a bare word throws.
        auto document = YAML::Load(*contents);
        if (!document.IsMap() && !document.IsNull())
            return std::unexpected(
                std::format("{}: the document is not a map of settings", core::pathToUtf8(file)));
        return document;
    }
    catch (std::exception const& e)
    {
        return std::unexpected(std::format("{}: {}", core::pathToUtf8(file), e.what()));
    }
}

std::string YAMLConfigReader::keyPathTo(YAML::Node const& node, std::string_view entry) const
{
    auto path = std::string {};
    auto joiner = std::string_view {};
    for (auto const& step: _keyPath)
    {
        path += joiner;
        path += step.entry;
        joiner = step.kind == KeyStepKind::File ? ": " : ".";
    }

    // The key a guarded read is on is its last step already; a key read inside its value is one more.
    auto const isLastStep =
        !_keyPath.empty() && _keyPath.back().entry == entry && _keyPath.back().container.is(node);
    if (!isLastStep)
    {
        path += joiner;
        path += entry;
    }
    return path;
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, CursorConfig& where)
{
    if (auto const child = node[entry])
    {
        loadFromEntry(child, "shape", where.cursorShape);
        loadFromEntry(child, "blinking", where.cursorDisplay);
        loadFromEntry(child, "blinking_interval", where.cursorBlinkInterval);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, InputModeConfig& where)
{
    // Subscripted here, under this entry's own guard: a mode given a scalar is that mode's fault.
    if (auto const child = node[entry]; child && child["cursor"])
        loadGuarded(child, "cursor", where.cursor);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, TerminalProfile& where)
{
    logger()("loading profile {}\n", entry);
    loadProfileBody(node[entry], where, entry);
}

void YAMLConfigReader::loadProfileBody(YAML::Node const& child,
                                       TerminalProfile& where,
                                       std::string_view profileName)
{
    if (child)
    {
        if (child["ssh"])
        {
            loadFromEntry(child, "ssh", where.ssh);
        }
        else
        {
            // load shell config if ssh is not provided
            loadFromEntry(child, "shell", where.shell);
        }
        // enforce some default shell setup
        defaultSettings(where.shell.value());

        loadFromEntry(child, "escape_sandbox", where.escapeSandbox);
        loadFromEntry(child, "copy_last_mark_range_offset", where.copyLastMarkRangeOffset);
        loadFromEntry(child, "copy_command_preserve_multiline", where.copyCommandPreserveMultiline);
        loadFromEntry(child, "show_title_bar", where.showTitleBar);
        loadFromEntry(child, "window_shadow", where.windowShadow);
        loadFromEntry(child, "dim_unfocused", where.dimUnfocused);
        loadFromEntry(child, "size_indicator_on_resize", where.sizeIndicatorOnResize);
        loadFromEntry(child, "fullscreen", where.fullscreen);
        loadFromEntry(child, "maximized", where.maximized);
        // search_mode_switch chose the Vi mode to return to after a search. Entering a search no
        // longer changes the mode -- the find bar floats over the terminal instead of living in the
        // status line -- so there is nothing left for it to select. Warn on presence rather than
        // parse-and-ignore, for the reason images.max_width states: a silently ignored setting is
        // worse than a removed one, because the user has no way to learn it stopped meaning anything.
        if (child["search_mode_switch"])
            errorLog()("Config entry profiles.*.search_mode_switch is deprecated and has no effect. "
                       "Search is typed into the find bar now, which changes no Vi mode. Remove the "
                       "entry to silence this warning.");

        loadFromEntry(child, "search_case_sensitivity", where.searchCaseSensitivity);
        loadFromEntry(child, "insert_after_yank", where.insertAfterYank);
        loadFromEntry(child, "bell", where.bell);
        loadFromEntry(child, "notify_on_command_finish", where.notifyOnCommandFinish);
        loadFromEntry(child, "wm_class", where.wmClass);
        loadFromEntry(child, "tab_label", where.tabLabel);
        loadFromEntry(child, "tab_color", where.tabColor);
        loadFromEntry(child, "pixel_reporting", where.pixelReporting);
        loadFromEntry(child, "option_as_alt", where.optionKeyAsAlt);
        loadFromEntry(child, "margins", where.margins);
        loadFromEntry(child, "terminal_id", where.terminalId);
        loadFromEntry(child, "frozen_dec_modes", where.frozenModes);
        loadFromEntry(child, "slow_scrolling_time", where.smoothLineScrolling);
        loadFromEntry(child, "smooth_scrolling", where.smoothScrolling);
        loadFromEntry(child, "momentum_scrolling", where.momentumScrolling);
        loadFromEntry(child, "terminal_size", where.terminalSize);
        loadFromEntry(child, "history", where.history);
        loadFromEntry(child, "scrollbar", where.scrollbar);
        loadFromEntry(child, "sticky_header", where.stickyHeader);
        loadFromEntry(child, "mouse", where.mouse);
        loadFromEntry(child, "permissions", where.permissions);
        loadFromEntry(child, "input_method_editor", where.inputMethodEditor);
        loadFromEntry(child, "highlight_word_and_matches_on_double_click", where.highlightDoubleClickedWord);
        loadFromEntry(child, "font", where.fonts);
        loadFromEntry(child, "draw_bold_text_with_bright_colors", where.drawBoldTextWithBrightColors);
        loadFromEntry(child, "blink_style", where.blinkStyle);
        loadFromEntry(child, "screen_transition", where.screenTransitionStyle);
        loadFromEntry(child, "screen_transition_duration", where.screenTransitionDuration);
        loadFromEntry(child, "cursor_motion_animation_duration", where.cursorMotionAnimationDuration);
        // Maps that are no config entries of their own, each guarded on its own: a value of the wrong
        // shape costs that key, not the whole profile.
        loadGuarded(child, "cursor", where.modeInsert.value().cursor);
        loadGuarded(child, "normal_mode", where.modeNormal.value());
        loadGuarded(child, "visual_mode", where.modeVisual.value());
        loadFromEntry(child, "vi_mode_highlight_timeout", where.highlightTimeout);
        loadFromEntry(child, "vi_mode_scrolloff", where.modalCursorScrollOff);
        loadFromEntry(child, "status_line", where.statusLine);
        loadFromEntry(child, "background", where.background);
        // clang-format on

        loadFromEntry(child, "colors", where.colors);

        if (auto* simple = get_if<SimpleColorConfig>(&(where.colors.value())))
            simple->colors.useBrightColors = where.drawBoldTextWithBrightColors.value();
        else if (auto* dual = get_if<DualColorConfig>(&(where.colors.value())))
        {
            dual->darkMode.useBrightColors = where.drawBoldTextWithBrightColors.value();
            dual->lightMode.useBrightColors = where.drawBoldTextWithBrightColors.value();
        }

        loadFromEntry(child, "hyperlink_decoration", where.hyperlinkDecoration);
        loadFromEntry(child, "link_detection", where.linkDetection);
        loadFromEntry(child, "file_opener", where.fileOpener);
        loadFromEntry(child, "hint_patterns", where.hintPatterns);
        loadFromEntry(child, "extensions", where.extensions);
        // Reported here, once, where the profile's own hint_patterns are read: a profile that merely inherits
        // another's patterns does not repeat that profile's report, and the table itself is compiled again,
        // silently, whenever the profile is applied.
        if (child["hint_patterns"])
            for (auto const& diagnostic:
                 vtbackend::LinkPatterns::compile(linkPatternSpecs(where)).diagnostics())
                errorLog()("Profile '{}': {}", profileName, diagnostic.message());
        loadFromEntry(child, "hint_scrollback_lines", where.hintScrollbackLines);
    }
}

void YAMLConfigReader::parseLayoutPane(YAML::Node const& node, config::LayoutPane& where)
{
    // Read `ratio:` FIRST: it applies to leaves and split nodes alike (a nested split is itself a
    // child of its parent split and may carry a size), and the split branch below returns early.
    // decode() (not as<double>()) so a typo like `ratio: half` is a logged, skipped field — not an
    // exception that unwinds the whole config load.
    if (auto const ratio = node["ratio"]; ratio && ratio.IsScalar())
    {
        double value = 0.0;
        if (YAML::convert<double>::decode(ratio, value) && value > 0.0 && value <= 1.0)
            where.ratio = value;
        else
            logger()("Invalid layout pane ratio '{}' (expected a number in (0, 1]); ignoring.",
                     ratio.as<std::string>());
    }

    if (auto const split = node["split"]; split && split.IsMap())
    {
        if (auto const orientation = split["orientation"]; orientation && orientation.IsScalar())
        {
            auto const value = core::toLower(orientation.as<std::string>());
            if (value == "horizontal")
                where.orientation = vtworkspace::SplitState::Horizontal;
            else if (value == "vertical")
                where.orientation = vtworkspace::SplitState::Vertical;
            else
                logger()("Unknown split orientation '{}' (expected 'horizontal' or 'vertical'); "
                         "using vertical.",
                         orientation.as<std::string>());
        }
        if (auto const panes = split["panes"]; panes && panes.IsSequence())
            for (auto const& paneNode: panes)
            {
                config::LayoutPane child;
                parseLayoutPane(paneNode, child);
                where.children.push_back(std::move(child));
            }
        // A single-child split has nothing to split against; collapse it into that one child so
        // it behaves as a plain leaf instead of spawning a bogus uncommanded second pane.
        if (where.children.size() == 1)
        {
            auto only = std::move(where.children.front());
            where = std::move(only);
        }
        return;
    }

    if (auto const command = node["command"]; command && command.IsScalar())
    {
        // Kept as written: the spawn reads it as it reads a profile's `shell:` -- a program, or, where it
        // names none but holds whitespace ("emacs -nw"), the command line it spells, by this platform's
        // rules -- with any `arguments:` after it. A blank one names nothing, and leaves the shell to run.
        auto text = resolveVariables(command.as<std::string>());
        if (text.find_first_not_of(" \t") != std::string::npos)
            where.command = std::move(text);
    }
    if (auto const args = node["arguments"]; args && args.IsSequence())
        for (auto const& argNode: args)
        {
            if (argNode.IsScalar())
                where.arguments.emplace_back(resolveVariables(argNode.as<std::string>()));
            else
                logger()("Ignoring non-scalar layout pane argument.");
        }
    if (auto const directory = node["directory"]; directory && directory.IsScalar())
        // resolvedPath (not bare homeResolvedPath): a layout's directory expands ${VAR} as well as
        // ~, exactly like every other path in the configuration.
        where.directory = resolvedPath(directory.as<std::string>());
    if (auto const profile = node["profile"]; profile && profile.IsScalar())
        where.profile = profile.as<std::string>();
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, config::Layout& where)
{
    auto const child = node[entry];
    if (!child || !child.IsMap())
        return;
    parseLayoutNode(child, where);
}

void YAMLConfigReader::parseLayoutNode(YAML::Node const& layoutNode, config::Layout& where)
{
    auto const tabs = layoutNode["tabs"];
    if (!tabs || !tabs.IsSequence())
        return;
    for (auto const& tabNode: tabs)
    {
        config::LayoutTab tab;
        if (auto const title = tabNode["title"]; title && title.IsScalar())
            tab.title = title.as<std::string>();
        if (auto const color = tabNode["color"]; color && color.IsScalar())
        {
            // Validate the format RGBColor actually accepts ('#RRGGBB' or 0x-prefixed); its
            // string assignment silently leaves the color black otherwise, which would render
            // a wrong (and then re-saved) tab color with no hint at the cause.
            auto const value = color.as<std::string>();
            if ((value.size() == 7 && value.front() == '#') || value.starts_with("0x"))
                tab.color = vtbackend::RGBColor { value };
            else
                logger()("Invalid layout tab color '{}' (expected '#RRGGBB'); ignoring.", value);
        }
        if (auto const profile = tabNode["profile"]; profile && profile.IsScalar())
            tab.profile = profile.as<std::string>();
        parseLayoutPane(tabNode, tab.root);
        where.tabs.push_back(std::move(tab));
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::unordered_map<std::string, config::Layout>& where)
{
    if (auto const child = node[entry]; child && child.IsMap())
    {
        for (auto layoutEntry: child)
        {
            if (!layoutEntry.first.IsScalar())
            {
                logger()("Ignoring layout with a non-scalar name.");
                continue;
            }
            auto const name = layoutEntry.first.as<std::string>();
            // Parse THIS entry's value node, not a re-lookup by name: yaml-cpp yields duplicate
            // keys once each, and a by-name lookup would find the first definition twice,
            // doubling its tabs. Later duplicates deterministically replace earlier ones.
            auto& layout = where[name];
            if (!layout.tabs.empty())
            {
                logger()("Duplicate layout '{}'; the later definition wins.", name);
                layout.tabs.clear();
            }
            parseLayoutNode(layoutEntry.second, layout);
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::map<std::string, HostEntry>& where) const
{
    auto const child = node[entry];
    if (!child || !child.IsMap())
        return;
    for (auto hostEntry: child)
    {
        if (!hostEntry.first.IsScalar())
        {
            errorLog()("Ignoring a host with a non-scalar name.");
            continue;
        }
        auto const name = hostEntry.first.as<std::string>();
        if (auto host = parseHostNode(hostEntry.second, name))
            where.insert_or_assign(name, std::move(*host));
    }
}

std::optional<HostEntry> YAMLConfigReader::parseHostNode(YAML::Node const& hostNode,
                                                         std::string const& name) const
{
    auto const skip = [&](std::string_view reason) -> std::optional<HostEntry> {
        errorLog()("Ignoring host '{}': {}", name, reason);
        return std::nullopt;
    };
    auto const scalar = [&](char const* key) -> std::optional<std::string> {
        if (auto const value = hostNode[key]; value && value.IsScalar())
            return value.as<std::string>();
        return std::nullopt;
    };
    auto const path = [&](char const* key) {
        return scalar(key)
            .transform([&](std::string const& text) { return resolvedPath(text); })
            .value_or(std::filesystem::path {});
    };

    if (!hostNode.IsMap())
        return skip("expected a map of url, token_file, tls_ca, remote_command and profile");
    auto const urlText = scalar("url");
    if (!urlText)
        return skip("it has no url");
    auto url = parseHostUrl(*urlText);
    if (!url)
        return skip(describe(url.error()));

    auto host = HostEntry {
        .url = std::move(*url),
        .tokenFile = path("token_file"),
        .tlsCa = path("tls_ca"),
        .remoteCommand = scalar("remote_command").value_or(std::string {}),
        .profile = scalar("profile").value_or(std::string {}),
    };
    // A TCP daemon always requires a token, so a saved TLS host without one could never attach.
    if (host.url.scheme == HostScheme::Tls && host.tokenFile.empty())
        return skip("a tls:// host needs a token_file");
    if (host.url.scheme == HostScheme::Tls && host.tlsCa.empty())
        errorLog()("Host '{}' has no tls_ca: the daemon's certificate is not verified", name);
    return host;
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::ColorPalette& where)
{
    logger()("color palette loading {}", entry);
    auto child = node[entry];

    if (vtbackend::defaultColorPalettes(entry, where))
    {
        logger()("Loaded predefined color palette {}", entry);
    }

    if (!child) // can not load directly from config file
    {
        logger()(
            "color paletter not found inside config file, checking colorschemes directory for {}.yml file",
            entry);
        auto const filePath = configFile.remove_filename() / "colorschemes" / sideFileName(entry);
        std::error_code ec;
        if (!std::filesystem::exists(filePath, ec))
        {
            logger()("color palette loading failed {} ", entry);
            return;
        }
        // A scheme file that is there but does not parse is the user's to fix, so it is reported where
        // every malformed entry is; the palette keeps what it has.
        auto const scheme = parseSideFile(filePath);
        if (!scheme)
        {
            errorLog()("Ignoring the color scheme file {}", scheme.error());
            return;
        }
        logger()("color palette loading from file {}", core::pathToUtf8(filePath));
        // Read as a document of its own, so a malformed map of it is reported by the file and its key
        // path there -- `<file>: <map>` -- rather than by the profile entry that named the scheme.
        readSideFile(filePath, *scheme, [&]() { loadFromEntry(*scheme, where); });
        return;
    }

    // Read where the document defines it, so a malformed map of it is reported by the key path it has
    // there -- `color_schemes.<name>.<map>` -- rather than by the profile entry that named the scheme.
    auto profilePath = std::exchange(_keyPath,
                                     std::vector { KeyStep { .container = doc, .entry = "color_schemes" },
                                                   KeyStep { .container = node, .entry = entry } });
    auto const restore = core::Finally { [&]() { _keyPath = std::move(profilePath); } };
    loadFromEntry(child, where);
}

namespace
{
    /// A colour scheme's `block_status` keys and the outcomes they colour. One table for the reader and
    /// the writer, so the two cannot come to spell a key differently, and a fourth status is one row here.
    constexpr auto BlockStatusKeys = std::array {
        std::pair { "success"sv, vtbackend::CommandBlockOutcome::Success },
        std::pair { "failure"sv, vtbackend::CommandBlockOutcome::Failure },
        std::pair { "running"sv, vtbackend::CommandBlockOutcome::Running },
    };

    /// A colour scheme's `sticky_header` keys and the palette slots they set, for BlockStatusKeys' reason:
    /// one table for the reader and the writer, so a third slot is a row here and a member of the palette.
    constexpr auto StickyHeaderColorSlots = std::array {
        std::pair { "background"sv, &vtbackend::ColorPalette::stickyHeaderBackground },
        std::pair { "separator"sv, &vtbackend::ColorPalette::stickyHeaderSeparator },
    };
} // namespace

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, vtbackend::ColorPalette& where)
{
    // Every map of a scheme is read under a guard of its own, as a profile's entries are: a map of the wrong
    // shape costs that map, which keeps its default, and is reported by its key path and its place in the
    // file, while the rest of the scheme still loads. Without a key, a map is not read at all.
    auto const loadMap = [&](std::string const& key, auto const& read) {
        auto const map = node[key];
        if (!map)
            return;
        logger()("*** loading {}", key);
        readGuarded(node, key, where, [&]() { read(map); });
    };

    loadMap("default", [&](YAML::Node const& map) {
        loadFromEntry(map, "background", where.defaultBackground);
        loadFromEntry(map, "foreground", where.defaultForeground);
        loadFromEntry(map, "bright_foreground", where.defaultForegroundBright);
        loadFromEntry(map, "dimmed_foreground", where.defaultForegroundDimmed);
    });

    loadMap("background_image", [&](YAML::Node const& map) {
        if (!map["path"]) // ensure that path exist
            return;
        where.backgroundImage = std::make_shared<vtbackend::BackgroundImage>();
        loadFromEntry(node, "background_image", where.backgroundImage);
    });

    loadMap("hyperlink_decoration", [&](YAML::Node const& map) {
        loadFromEntry(map, "normal", where.hyperlinkDecoration.normal);
        loadFromEntry(map, "hover", where.hyperlinkDecoration.hover);
    });

    auto const loadEntry = [&](std::string const& key, auto& slot) {
        loadMap(key, [&](YAML::Node const&) { loadFromEntry(node, key, slot); });
    };
    loadEntry("cursor", where.cursor);
    loadEntry("vi_mode_highlight", where.yankHighlight);
    loadEntry("vi_mode_cursorline", where.normalModeCursorline);
    loadEntry("selection", where.selection);
    loadEntry("search_highlight", where.searchHighlight);
    loadEntry("search_highlight_focused", where.searchHighlightFocused);
    loadEntry("word_highlight_current", where.wordHighlightCurrent);
    loadEntry("word_highlight_other", where.wordHighlight);
    loadEntry("hint_label", where.hintLabel);
    loadEntry("hint_match", where.hintMatch);
    loadEntry("input_method_editor", where.inputMethodEditor);

    loadMap("indicator_statusline", [&](YAML::Node const& map) {
        if (map["default"])
        {
            logger()("*** loading default indicator_statusline");
            vtbackend::RGBColorPair defaultIndicatorStatusLine;
            loadFromEntry(map, "default", defaultIndicatorStatusLine);
            where.indicatorStatusLineInactive = defaultIndicatorStatusLine;
            where.indicatorStatusLineInsertMode = defaultIndicatorStatusLine;
            where.indicatorStatusLineNormalMode = defaultIndicatorStatusLine;
            where.indicatorStatusLineVisualMode = defaultIndicatorStatusLine;
        }
        for (auto const& [name, defaultColor]:
             { pair { "inactive", &where.indicatorStatusLineInactive },
               pair { "insert_mode", &where.indicatorStatusLineInsertMode },
               pair { "normal_mode", &where.indicatorStatusLineNormalMode },
               pair { "visual_mode", &where.indicatorStatusLineVisualMode } })
        {
            if (map[name])
            {
                logger()("*** loading {} indicator_statusline", name);
                loadFromEntry(map, name, *defaultColor);
            }
        }
    });

    // The fold column, left unset unless the scheme says otherwise: vtbackend then derives it from the
    // palette's own foreground and background, so a scheme that has never heard of folding still draws
    // a legible column rather than one hard-coded colour that happens to vanish on half the themes.
    loadMap("fold_marker", [&](YAML::Node const& map) {
        loadOptional(map, "default", where.foldMarker);
        loadOptional(map, "hover", where.foldMarkerHover);
    });

    // The block column's status colours and the gutter's text colour, each left unset unless the scheme
    // says otherwise, for the fold column's reason: vtbackend derives them from the palette
    // (ColorPalette::blockStatusColor, gutterTextColor), so a scheme that has never heard of them still
    // shows them.
    loadMap("block_status", [&](YAML::Node const& map) {
        for (auto const& [key, outcome]: BlockStatusKeys)
            loadOptional(map, key, where.blockStatus[static_cast<size_t>(outcome)]);
    });
    loadMap("block_status_hover", [&](YAML::Node const& map) {
        for (auto const& [key, outcome]: BlockStatusKeys)
            loadOptional(map, key, where.blockStatusHover[static_cast<size_t>(outcome)]);
    });
    loadMap("gutter_text", [&](YAML::Node const&) { loadOptional(node, "gutter_text", where.gutterText); });

    // The sticky header's band and the line along its bottom edge, each left unset unless the scheme says
    // otherwise, for the fold column's reason (ColorPalette::stickyHeaderBackgroundColor,
    // stickyHeaderSeparatorColor).
    loadMap("sticky_header", [&](YAML::Node const& map) {
        for (auto const& [key, slot]: StickyHeaderColorSlots)
            loadOptional(map, key, where.*slot);
    });

    // Driven by ContextTypeList, so a thirteenth context type is one row in vtbackend and nothing here.
    // Lenient in the same way the sequence parser is: an unknown key is skipped and every other entry still
    // loads.
    loadMap("tint", [&](YAML::Node const& map) {
        for (auto const type: vtbackend::ContextTypeList)
            loadOptional(
                map, vtbackend::contextTypeName(type), where.contextTints[static_cast<size_t>(type)]);
    });

    logger()("*** loading palette");
    loadFromEntry(node, "", where.palette);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     [[maybe_unused]] std::string const& entry,
                                     vtbackend::ColorPalette::Palette& colors)
{
    auto const loadColorMap = [&](YAML::Node const& parent, std::string const& key, size_t offset) -> bool {
        auto node = parent[key];
        if (!node)
            return false;

        // One spelling rule for both forms: `#rrggbb`, or `0xrrggbb` as a number.
        auto const assignColor = [&](size_t index, YAML::Node const& nodeValue) {
            if (auto const value = nodeValue.as<std::string>(); !value.empty())
            {
                if (value[0] == '#')
                    colors[offset + index] = value;
                else if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
                    colors[offset + index] = vtbackend::RGBColor { nodeValue.as<uint32_t>() };
            }
        };

        if (node.IsMap())
        {
            for (auto const [index, name]: core::views::enumerate(
                     std::array { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white" }))
                if (auto const nodeValue = node[name])
                    assignColor(static_cast<size_t>(index), nodeValue);
            return true;
        }
        if (node.IsSequence())
        {
            for (auto const index: std::views::iota(size_t { 0 }, std::min(node.size(), size_t { 8 })))
                assignColor(index, node[index]);
            return true;
        }
        return false;
    };

    // Each map under a guard of its own, as the scheme's other maps are.
    auto const loadGuardedColorMap = [&](std::string const& key, size_t offset) {
        auto loaded = false;
        readGuarded(node, key, colors, [&]() { loaded = loadColorMap(node, key, offset); });
        return loaded;
    };

    logger()("*** loading normal color map");
    loadGuardedColorMap("normal", 0);
    logger()("*** loading bright color map");
    loadGuardedColorMap("bright", 8);
    logger()("*** loading dim color map");
    if (!loadGuardedColorMap("dim", 256))
    {
        // calculate dim colors based on normal colors
        for (unsigned i = 0; i < 8; ++i)
            colors[256 + i] = colors[i] * 0.5f;
    }
    logger()("*** color palette is loaded");
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::CellRGBColorAndAlphaPair& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "foreground", where.foreground);
        loadFromEntry(child, "foreground_alpha", where.foregroundAlpha);
        loadFromEntry(child, "background", where.background);
        loadFromEntry(child, "background_alpha", where.backgroundAlpha);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::RGBColorPair& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "foreground", where.foreground);
        loadFromEntry(child, "background", where.background);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::RGBColor& where)
{
    auto const child = node[entry];
    if (child)
        where = child.as<std::string>();
    logger()("Loading entry: {}, value {}", entry, where);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::optional<vtbackend::RGBColor>& where)
{
    auto const child = node[entry];
    if (!child)
        return;
    if (child.IsNull())
    {
        where.reset();
        return;
    }
    auto const value = child.as<std::string>();
    if (auto const color = vtbackend::parseColor(value); color && value.size() == 7 && value.front() == '#')
        where = color;
    else
        logger()("Invalid tab color '{}' (expected '#RRGGBB' or null); ignoring.", value);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::CursorColor& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "default", where.color);
        loadFromEntry(child, "text", where.textOverrideColor);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::CellRGBColor& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::CellRGBColor> {
        auto const literal = core::toUpper(key);
        logger()("Loading entry: {}, value {}", entry, where);
        if (literal == "CELLBACKGROUND")
            return vtbackend::CellBackgroundColor {};
        if (literal == "CELLFOREGROUND")
            return vtbackend::CellForegroundColor {};
        return vtbackend::RGBColor(key);
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::shared_ptr<vtbackend::BackgroundImage>& where)
{
    logger()("Loading background_image");

    auto const child = node[entry];

    if (child)
    {
        std::string filename;
        loadFromEntry(child, "path", filename);
        loadFromEntry(child, "opacity", where->opacity);
        loadFromEntry(child, "blur", where->blur);
        auto const resolved = resolvedPath(filename);
        where->location = resolved;
        where->hash = crispy::StrongHash::compute(core::pathToUtf8(resolved));
    }
}

namespace
{
    /// Reads a permission's spelling, ignoring case.
    /// @param token One of `allow`, `deny` and `ask`.
    /// @return The permission, or no value for any other spelling.
    [[nodiscard]] std::optional<Permission> permissionFromToken(std::string_view token)
    {
        auto const literal = core::toLower(token);
        if (literal == "allow")
            return Permission::Allow;
        if (literal == "deny")
            return Permission::Deny;
        if (literal == "ask")
            return Permission::Ask;
        return std::nullopt;
    }
} // namespace

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, Permission& where)
{
    if (auto const child = node[entry]; child)
    {
        if (auto const opt = permissionFromToken(child.as<std::string>()); opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::StatusDisplayType& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::StatusDisplayType> {
        auto const literal = core::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "indicator")
            return vtbackend::StatusDisplayType::Indicator;
        if (literal == "none")
            return vtbackend::StatusDisplayType::None;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        if (auto const opt = parseModifierKey(child.as<std::string>()); opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::Opacity& where)
{
    if (auto const child = node[entry])
    {
        where = vtbackend::Opacity(static_cast<unsigned>(255 * std::clamp(child.as<float>(), 0.0f, 1.0f)));
    }
    logger()("Loading entry: {}, value {}", entry, static_cast<unsigned>(where));
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, Bell& where)
{
    if (auto const child = node[entry])
    {
        loadFromEntry(child, "alert", where.alert);
        loadFromEntry(child, "sound", where.sound);
        loadFromEntry(child, "volume", where.volume);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtpty::SshHostConfig& where)
{
    if (auto const child = node[entry])
    {
        loadFromEntry(child, "host", where.hostname);
        loadFromEntry(child, "port", where.port);
        loadFromEntry(child, "user", where.username);
        loadFromEntry(child, "private_key", where.privateKeyFile);
        loadFromEntry(child, "public_key", where.publicKeyFile);
        loadFromEntry(child, "known_hosts", where.knownHostsFile);
        loadFromEntry(child, "forward_agent", where.forwardAgent);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtpty::Process::ExecInfo& where) const
{
    if (auto const child = node[entry])
    {
        where.program = resolveVariables(child.as<std::string>());
    }
    // loading arguments from the profile
    if (auto args = node["arguments"]; args && args.IsSequence())
    {
        for (auto const& argNode: args)
            where.arguments.emplace_back(resolveVariables(argNode.as<string>()));
    }
    if (node["initial_working_directory"])
    {
        loadFromEntry(node, "initial_working_directory", where.workingDirectory);
    }
    else
    {
        // In UTF-8, which homeResolvedPath() reads: generic_string() would spell it in the ANSI code page on
        // Windows.
        where.workingDirectory =
            homeResolvedPath(core::platform::normalizePath(where.workingDirectory), Process::homeDirectory())
                .value_or(where.workingDirectory);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::vector<text::FontFeature>& where)
{
    if (auto child = node[entry])
    {
        for (auto&& feature: child)
        {
            // Feature can be either 4 letter code or optionally ending with - to denote disabling it.
            auto const [tag, enabled] = [&]() -> tuple<string, bool> {
                auto value = feature.as<string>();
                if (!value.empty())
                {
                    if (value[0] == '+')
                        return { value.substr(1), true };
                    if (value[0] == '-')
                        return { value.substr(1), false };
                }
                return { std::move(value), true };
            }();

            if (tag.size() != 4)
            {
                logger()("Invalid font feature \"{}\". Font features are denoted as 4-letter codes.",
                         feature.as<string>());
                continue;
            }
            logger()("Enabling font feature {}{}{}{}", tag[0], tag[1], tag[2], tag[3]);
            where.emplace_back(tag[0], tag[1], tag[2], tag[3], enabled);
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::set<std::string>& where)
{
    if (auto child = node[entry]; child && child.IsMap())
    {
        // entries of kind  feature_xyz: true
        for (auto const& feature: child)
        {
            auto const isEnabled = feature.second.as<bool>();
            if (isEnabled)
            {
                where.insert(feature.first.as<std::string>());
                logger()("Added feature {}", feature.first.as<std::string>());
            }
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::map<vtbackend::DECMode, bool>& where)
{

    if (auto frozenDecModes = node[entry]; frozenDecModes)
    {
        if (frozenDecModes.IsMap())
        {
            for (auto const& modeNode: frozenDecModes)
            {
                auto const modeNumber = std::stoi(modeNode.first.as<string>());
                if (!vtbackend::isValidDECMode(modeNumber))
                {
                    logger()("Invalid frozen_dec_modes entry: {} (Invalid DEC mode number).", modeNumber);
                    continue;
                }
                auto const mode = static_cast<vtbackend::DECMode>(modeNumber);
                auto const frozenState = modeNode.second.as<bool>();
                where[mode] = frozenState;
            }
        }
        else
            logger()("Invalid frozen_dec_modes entry.");
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::LineOffset& where)
{
    if (auto const child = node[entry])
        where = vtbackend::LineOffset(child.as<int>());
    logger()("Loading entry: {}, value {}", entry, where.template as<int>());
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, WindowMargins& where)
{
    if (auto const child = node[entry])
    {
        loadFromEntry(child, "horizontal", where.horizontal);
        loadFromEntry(child, "vertical", where.vertical);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::PageSize& where)
{
    if (auto const child = node[entry])
    {
        loadFromEntry(child, "lines", where.lines);
        loadFromEntry(child, "columns", where.columns);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::VTType& where)
{
    auto parseModifierKey = [](std::string const& key) -> std::optional<vtbackend::VTType> {
        // Compare case-insensitively: `key` is lowercased below, so the mapping keys must be too —
        // otherwise NO terminal_id value ever matches and the setting is silently ignored (the
        // mappings were uppercase while the lookup key was lowercased).
        auto const literal = core::toLower(key);

        using Type = vtbackend::VTType;
        auto constexpr static Mappings = std::array<std::pair<std::string_view, Type>, 10> {
            std::pair { "vt100", Type::VT100 }, std::pair { "vt220", Type::VT220 },
            std::pair { "vt240", Type::VT240 }, std::pair { "vt330", Type::VT330 },
            std::pair { "vt340", Type::VT340 }, std::pair { "vt320", Type::VT320 },
            std::pair { "vt420", Type::VT420 }, std::pair { "vt510", Type::VT510 },
            std::pair { "vt520", Type::VT520 }, std::pair { "vt525", Type::VT525 }
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == literal)
                return mapping.second;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::LineCount& where)
{
    if (auto const child = node[entry])
        where = vtbackend::LineCount(child.as<int>());
}

namespace
{
    /// The decorator names a configuration file may use, comma separated, for a diagnostic.
    [[nodiscard]] std::string acceptedDecoratorNames()
    {
        auto names = std::string {};
        for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
        {
            if (!names.empty())
                names += ", ";
            names += token;
        }
        return names;
    }
} // namespace

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::Decorator& where)
{
    // The spellings are vtrasterizer::DecoratorNames' (and its aliases'), the table the writer and the
    // settings page read too; a miss is reported and keeps the value `where` already holds.
    (void) detail::loadConfigEnum(
        node,
        entry,
        keyPathTo(node, entry),
        where,
        logger,
        [](std::string const& token) { return vtrasterizer::toDecorator(core::toLower(token)); },
        acceptedDecoratorNames());
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontSize& where)
{
    if (auto const child = node[entry])
    {
        auto const size = child.as<double>();
        if (size < MinimumFontSize.pt)
        {
            logger()("Specified font size is smaller than minimal available 8");
            where = MinimumFontSize;
            return;
        }
        where.pt = size;
    }
    logger()("Loading entry: {}, value {}", entry, where.pt);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontSlant& where)
{
    // `thin` is a weight, not a slant; it reached the slant table as a copy-paste and resolved to
    // Normal for years. Keep honouring it so existing configs do not change behaviour, but say so --
    // silently ignoring it would be worse here than elsewhere, because the italic and bold-italic
    // faces are pre-seeded with FontSlant::Italic before this runs, so a dropped value leaves the
    // inherited Italic rather than the default.
    //
    // Checked before the reader runs, not after: reporting it as unknown first and honouring it
    // second would tell the user both that the value was rejected and that it was accepted.
    if (auto const child = node[entry]; child && text::namesMatch(child.as<std::string>(), "thin"))
    {
        errorLog()(R"(Setting {} to "thin" is deprecated and means "normal".)", entry);
        where = text::FontSlant::Normal;
        return;
    }

    detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger, text::makeFontSlant);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     text::FontWeight& where)
{
    detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger, text::makeFontWeight);
}
// NOLINTEND(readability-convert-member-functions-to-static)

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     text::FontDescription& where)
{
    if (auto const child = node[entry])
    {
        if (child.IsMap())
        {
            loadFromEntry(child, "family", where.familyName);
            loadFromEntry(child, "weight", where.weight);
            loadFromEntry(child, "slant", where.slant);
            loadFromEntry(child, "features", where.features);

            if (child["fallback"])
            {
                if (child["fallback"].IsScalar() && (child["fallback"].as<std::string>() == "none"))
                {
                    where.fontFallback = text::FontFallbackNone {};
                }
                else if (child["fallback"].IsSequence())
                {
                    where.fontFallback = text::FontFallbackList {};
                    auto& list = std::get<text::FontFallbackList>(where.fontFallback);
                    for (auto&& fallback: child["fallback"])
                    {
                        list.fallbackFonts.emplace_back(fallback.as<std::string>());
                    }
                }
            }
        }
        else // entries like emoji: "emoji"
        {
            where.familyName = child.as<std::string>();
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, RendererConfig& where)
{
    // Each leaf under its own guard, as in every section settings.yml overrides: a leaf of the wrong type
    // costs itself, not the leaves set beside it.
    auto const child = node[entry];
    if (child)
    {
        loadGuarded(child, "tile_direct_mapping", where.textureAtlasDirectMapping);
        loadGuarded(child, "tile_hashtable_slots", where.textureAtlasHashtableSlots);
        loadGuarded(child, "tile_cache_count", where.textureAtlasTileCount);
        loadGuarded(child, "backend", where.renderingBackend);
        loadGuarded(child, "gpu", where.gpu);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, ImagesConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "sixel_scrolling", where.sixelScrolling);
        loadFromEntry(child, "sixel_register_count", where.maxImageColorRegisters);

        // max_width/max_height are deprecated: the image canvas is derived from the screen. The
        // keys still parse so existing configurations keep loading, but a silently ignored setting
        // is worse than a removed one -- the user has no way to learn it stopped meaning anything.
        // Warn on presence rather than on value: writing an explicit 0 is just as much a statement
        // about a key that no longer exists.
        for (auto const* deprecatedKey: { "max_width", "max_height" })
            if (child[deprecatedKey])
                errorLog()("Config entry images.{} is deprecated and has no effect. The maximum image "
                           "size is derived from the screen size. Remove the entry to silence this "
                           "warning.",
                           deprecatedKey);

        loadFromEntry(child, "good_image_protocol", where.goodImageProtocol);
    }
}

namespace
{
    /// The names of @p table, comma separated, for a diagnostic.
    template <typename Table>
    [[nodiscard]] std::string acceptedNames(Table const& table)
    {
        auto names = std::string {};
        for (auto const& [name, value]: table)
        {
            if (!names.empty())
                names += ", ";
            names += name;
        }
        return names;
    }
} // namespace

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, DaemonSettings& where)
{
    auto const child = node[entry];
    if (!child)
        return;

    (void) detail::loadConfigEnum(
        child,
        "mode",
        keyPathTo(child, "mode"),
        where.mode,
        logger,
        [](std::string const& token) { return daemonModeFrom(core::toLower(token)); },
        acceptedNames(DaemonModeNames));
    (void) detail::loadConfigEnum(
        child,
        "size_policy",
        keyPathTo(child, "size_policy"),
        where.sizePolicy,
        logger,
        [](std::string const& token) { return vthost::clientSizePolicyFrom(core::toLower(token)); },
        acceptedNames(vthost::ClientSizePolicyNames));
    loadGuarded(child, "socket", where.socket);

    // Absent or null (`~`) leaves the limit to the hosting profile's `history.limit`.
    if (auto const limit = child["history_limit"]; limit && !limit.IsNull())
    {
        auto value = vtbackend::MaxHistoryLineCount { vtbackend::LineCount(0) };
        loadFromEntry(child, "history_limit", value);
        where.historyLimit = value;
    }

    loadFromEntry(child, "restore", where.restore);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, HistoryConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "limit", where.maxHistoryLineCount);
        // Read raw, reconciled against the limit by HistoryConfig::limits(). Doing it there rather
        // than here means a profile with no `history:` node at all gets the same answer as one that
        // spells the defaults out -- this branch only runs when the node exists.
        loadFromEntry(child, "hard_limit", where.hardLimit);
        loadFromEntry(child, "scroll_multiplier", where.historyScrollMultiplier);
        loadFromEntry(child, "auto_scroll_on_update", where.autoScrollOnUpdate);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, FoldingConfig& where)
{
    // Each leaf under its own guard, for the renderer section's reason.
    auto const child = node[entry];
    if (child)
    {
        loadGuarded(child, "enabled", where.enabled);
        loadGuarded(child, "show_markers", where.showMarkers);
        loadGuarded(child, "auto_collapse_on_new_command", where.autoCollapseOnNewCommand);
        loadGuarded(child, "on_jump_into_fold", where.onJumpIntoFold);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, GutterConfig& where)
{
    // Only the children present are written: an override of one key must leave its siblings as they were.
    // Each under its own guard, for the renderer section's reason.
    auto const child = node[entry];
    if (!child)
        return;

    loadGuarded(child, "exit_status", where.exitStatus);
    loadGuarded(child, "user_marks", where.userMarks);
    loadGuarded(child, "bold_on_hover", where.boldOnHover);
    loadGuarded(child, "line_numbers", where.lineNumbers);
    loadGuarded(child, "timestamps", where.timestamps);

    // Validated rather than clamped or trusted: a width out of range and a format the gutter cannot draw
    // are typos, and either taken as written would resize the page or blank the column.
    struct BoundedWidth
    {
        char const* key;
        int GutterConfig::* member;
        int min;
        int max;
    };
    constexpr auto BoundedWidths = std::to_array<BoundedWidth>({
        { .key = "block_column_width",
          .member = &GutterConfig::blockColumnWidth,
          .min = vtbackend::MinBlockColumnWidth,
          .max = vtbackend::MaxBlockColumnWidth },
        { .key = "line_number_width",
          .member = &GutterConfig::lineNumberWidth,
          .min = vtbackend::MinLineNumberWidth,
          .max = vtbackend::MaxLineNumberWidth },
    });
    for (auto const& bounded: BoundedWidths)
    {
        if (!child[bounded.key])
            continue;
        auto& kept = where.*bounded.member;
        auto width = kept;
        loadGuarded(child, bounded.key, width);
        if (width >= bounded.min && width <= bounded.max)
            kept = width;
        else
            errorLog()("gutter.{} must be between {} and {}, not {}; keeping {}.",
                       bounded.key,
                       bounded.min,
                       bounded.max,
                       width,
                       kept);
    }

    if (child["timestamp_format"])
    {
        auto spec = where.timestampFormat;
        loadGuarded(child, "timestamp_format", spec);
        if (auto const measured = vtbackend::measureTimestampFormat(spec); measured)
            where.timestampFormat = std::move(spec);
        else
            errorLog()("gutter.timestamp_format '{}' cannot be used: {}; keeping '{}'.",
                       spec,
                       vtbackend::timestampFormatErrorText(measured.error()),
                       where.timestampFormat);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     OscContextConfig& where)
{
    auto const child = node[entry];
    if (!child)
        return;

    loadFromEntry(child, "enabled", where.enabled);
    loadFromEntry(child, "max_depth", where.maxDepth);
    loadFromEntry(child, "max_retained_contexts", where.maxRetained);
    loadFromEntry(child, "derive_markers", where.deriveMarkers);
    loadFromEntry(child, "tinting", where.tinting);

    // Clamped rather than rejected, and the ORDER matters: a retained bound below the depth bound
    // would let a context that is still an ancestor be evicted, leaving the chain holding a record
    // nothing can resolve -- which ContextStack's constructor treats as a programmer error.
    where.maxDepth = std::clamp(where.maxDepth, uint16_t { 1 }, uint16_t { 256 });
    where.maxRetained = std::max(where.maxRetained, where.maxDepth);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     CommandBlocksConfig& where)
{
    // Each leaf under its own guard, for the renderer section's reason.
    auto const child = node[entry];
    if (!child)
        return;

    // Read as a signed number and clamped, rather than parsed straight into size_t: a negative value would
    // otherwise make yaml-cpp throw, or wrap around to an effectively unbounded store.
    auto requested = static_cast<int64_t>(where.maxRecords);
    loadGuarded(child, "max_records", requested);
    where.maxRecords = static_cast<size_t>(std::clamp<int64_t>(requested, 1, MaxCommandBlockRecords));
    loadGuarded(child, "pager", where.pager);
    loadGuarded(child, "pager_format", where.pagerFormat);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "position", where.position);
        loadFromEntry(child, "hide_in_alt_screen", where.hideScrollbarInAltScreen);
        loadFromEntry(child, "marks", where.marks);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::ScrollbarMarkSources& where)
{
    auto const child = node[entry];
    if (!child)
        return;

    // A LIST of tokens, each naming one group of marks. `[]` is a deliberate "none", not a missing key, so an
    // empty list replaces the default rather than leaving it in place.
    if (!child.IsSequence())
    {
        errorLog()("{} ({}) takes a list, such as {}; keeping {}.",
                   keyPathTo(node, entry),
                   detail::placeOf(child),
                   Writer::format(vtbackend::AllScrollbarMarkSources),
                   Writer::format(where));
        return;
    }

    auto sources = vtbackend::ScrollbarMarkSources {};
    for (auto const& item: child)
    {
        if (!item.IsScalar())
        {
            errorLog()("{} ({}) lists something other than a word; ignored.",
                       keyPathTo(node, entry),
                       detail::placeOf(item));
            continue;
        }

        auto const token = item.as<std::string>();
        logger()("Loading entry: {}, value {}", entry, token);
        // Through the token table the writer spells the list from, so what it writes reads back.
        if (auto const source = configEnumFromToken<vtbackend::ScrollbarMarkSource>(token))
            sources.enable(*source);
        else
            errorLog()("Unknown value in {} ({}): '{}'; ignored.",
                       keyPathTo(node, entry),
                       detail::placeOf(item),
                       token);
    }
    where = sources;
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     StickyHeaderConfig& where)
{
    // Only the children present are written: an override of one key must leave its sibling as it was.
    auto const child = node[entry];
    if (!child)
        return;

    loadFromEntry(child, "mode", where.mode);
    loadFromEntry(child, "show_evicted", where.showEvicted);
}

namespace
{
    /// One `hint_patterns` entry as written.
    /// @param item The entry's mapping.
    /// @return The entry; or, where a key holds what it cannot take, what is wrong, naming the entry.
    [[nodiscard]] std::expected<HintPatternConfig, std::string> readHintPattern(YAML::Node const& item)
    {
        auto pattern = HintPatternConfig {};
        try
        {
            if (auto const nameNode = item["name"])
                pattern.name = nameNode.as<std::string>();
            if (auto const regexNode = item["regex"])
                pattern.regex = regexNode.as<std::string>();
            if (auto const clickableNode = item["clickable"])
                pattern.clickable = clickableNode.as<bool>();
            if (auto const urlNode = item["url"])
                pattern.url = urlNode.as<std::string>();
        }
        catch (std::exception const& e)
        {
            return std::unexpected { std::format(
                "pattern '{}': {}", pattern.name.empty() ? "<unnamed>" : pattern.name, e.what()) };
        }
        return pattern;
    }
} // namespace

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::vector<HintPatternConfig>& where) const
{
    auto const child = node[entry];
    if (child && child.IsSequence())
    {
        where.clear();
        for (auto const& item: child)
        {
            // A malformed key costs its own entry, not the list. A name-only entry changes a built-in's keys
            // and keeps its regex; any other needs one.
            auto pattern = readHintPattern(item);
            auto const reason =
                pattern ? skippedHintPatternReason(*pattern) : std::optional { pattern.error() };
            if (reason)
                errorLog()("Skipping an entry of {}: {}.", keyPathTo(node, entry), *reason);
            else
                where.push_back(std::move(*pattern));
        }
    }
}

namespace
{
    /// Reads an extension's `config:` map into @p extension: a key holding what it cannot take costs that
    /// key, and a key set twice keeps the later value.
    /// @param values The `config` node, a map or null.
    /// @param extension Receives the keys and values.
    /// @param keyPath Where the extension is declared, for the diagnostics.
    void readExtensionConfigValues(YAML::Node const& values,
                                   ExtensionConfig& extension,
                                   std::string_view keyPath)
    {
        if (values.IsNull())
            return;
        if (!values.IsMap())
        {
            errorLog()("Ignoring 'config' of extension '{}' of {}: expected a map of keys to values.",
                       extension.name,
                       keyPath);
            return;
        }
        for (auto const& item: values)
        {
            if (!item.first.IsScalar() || !item.second.IsScalar())
            {
                errorLog()("Ignoring a 'config' key of extension '{}' of {}: keys and values must be text.",
                           extension.name,
                           keyPath);
                continue;
            }
            auto key = item.first.as<std::string>();
            if (extension.config.contains(key))
                errorLog()("Extension '{}' of {} sets 'config' key '{}' twice; the later one stands.",
                           extension.name,
                           keyPath,
                           key);
            extension.config.insert_or_assign(std::move(key), item.second.as<std::string>());
        }
    }
} // namespace

std::optional<ExtensionConfig> YAMLConfigReader::readExtension(std::string name,
                                                               YAML::Node const& settings,
                                                               std::string_view keyPath) const
{
    auto extension = ExtensionConfig {};
    extension.name = std::move(name);
    if (settings.IsNull()) // `git:` alone: a bundled extension, with the defaults.
        return extension;
    if (!settings.IsMap())
    {
        errorLog()("Skipping extension '{}' of {}: expected a map of path, config and run_command.",
                   extension.name,
                   keyPath);
        return std::nullopt;
    }

    // A path that is given must name a module: falling back to the bundled extension of the same name
    // would run something else than the file asked for.
    if (auto const path = settings["path"]; path)
    {
        auto const text = path.IsScalar() ? path.as<std::string>() : std::string {};
        extension.path = text.empty() ? std::filesystem::path {} : resolvedPath(text);
        if (extension.path.empty())
        {
            errorLog()(
                "Skipping extension '{}' of {}: its 'path' must be a file name.", extension.name, keyPath);
            return std::nullopt;
        }
        if (extension.path.is_relative())
            extension.path = (declaringDirectory() / extension.path).lexically_normal();
    }

    if (auto const values = settings["config"]; values)
        readExtensionConfigValues(values, extension, keyPath);

    if (auto const runCommand = settings["run_command"]; runCommand)
    {
        auto const token = runCommand.IsScalar() ? runCommand.as<std::string>() : std::string {};
        if (auto const permission = permissionFromToken(token))
            extension.runCommand = *permission;
        else
            errorLog()(
                "Unknown run_command '{}' of extension '{}' of {}; expected allow, deny or ask. Asking.",
                token,
                extension.name,
                keyPath);
    }
    return extension;
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::vector<ExtensionConfig>& where) const
{
    auto const child = node[entry];
    if (!child)
        return;
    if (child.IsNull()) // `extensions:` with nothing under it: none.
    {
        where.clear();
        return;
    }
    auto const keyPath = keyPathTo(node, entry);
    if (!child.IsMap()) // Ignored whole: what the profile inherited stays.
    {
        errorLog()("Ignoring {}: expected a map from an extension's name to its settings.", keyPath);
        return;
    }
    where.clear();

    for (auto const& item: child)
    {
        if (!item.first.IsScalar() || item.first.as<std::string>().empty())
        {
            errorLog()("Skipping an extension of {}: its name must be a non-empty text.", keyPath);
            continue;
        }

        auto name = item.first.as<std::string>();
        if (std::ranges::any_of(where, [&](ExtensionConfig const& known) { return known.name == name; }))
        {
            errorLog()(
                "Skipping extension '{}' of {}: it is declared twice; the first one stands.", name, keyPath);
            continue;
        }

        if (auto extension = readExtension(std::move(name), item.second, keyPath))
            where.push_back(*std::move(extension));
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, MouseConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "hide_while_typing", where.hideWhileTyping);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     StatusLineConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "position", where.position);
        loadFromEntry(child, "sync_to_window_title", where.syncWindowTitleWithHostWritableStatusDisplay);
        loadFromEntry(child, "display", where.initialType);
        if (child["indicator"])
        {
            loadFromEntry(child["indicator"], "left", where.indicator.left);
            loadFromEntry(child["indicator"], "middle", where.indicator.middle);
            loadFromEntry(child["indicator"], "right", where.indicator.right);
        }
        // An absent key leaves the value as it is, a null or empty one resets it: the template-driven line.
        if (auto const extension = child["extension"]; extension)
        {
            if (extension.IsNull())
                where.extension.reset();
            else if (!extension.IsScalar())
                errorLog()("Ignoring {}: expected the name of an extension.", keyPathTo(child, "extension"));
            else if (auto name = extension.as<std::string>(); !name.empty())
                where.extension = std::move(name);
            else
                where.extension.reset();
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     BackgroundConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "opacity", where.opacity);
        loadFromEntry(child, "blur", where.blur);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     HyperlinkDecorationConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "mode", where.mode);
        loadFromEntry(child, "normal", where.normal);
        loadFromEntry(child, "hover", where.hover);
    }
}

namespace
{
    vtbackend::ScreenLinkDetection screenLinkDetectionOf(ScreenLinkDetectionConfig const& config) noexcept
    {
        auto const toSwitch = [](bool on) noexcept {
            return on ? vtbackend::DetectLinks::Yes : vtbackend::DetectLinks::No;
        };
        return vtbackend::ScreenLinkDetection { .urls = toSwitch(config.urls),
                                                .filePaths = toSwitch(config.filePaths) };
    }
} // namespace

vtbackend::LinkDetectionPolicy linkDetectionPolicyOf(LinkDetectionConfig const& config) noexcept
{
    return vtbackend::LinkDetectionPolicy { .primaryScreen = screenLinkDetectionOf(config.primaryScreen),
                                            .otherScreens = screenLinkDetectionOf(config.otherScreens) };
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     LinkDetectionConfig& where)
{
    auto const child = node[entry];
    if (!child)
        return;
    if (auto const group = child["primary_screen"]; group)
    {
        loadFromEntry(group, "urls", where.primaryScreen.urls);
        loadFromEntry(group, "file_paths", where.primaryScreen.filePaths);
    }
    if (auto const group = child["other_screens"]; group)
    {
        loadFromEntry(group, "urls", where.otherScreens.urls);
        loadFromEntry(group, "file_paths", where.otherScreens.filePaths);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     FileOpenerConfig& where)
{
    auto const child = node[entry];
    if (!child)
        return;
    // The string form names the command alone, which then runs detached -- also where the profile started as
    // a copy of one whose opener ran in a pane.
    if (child.IsScalar())
    {
        loadFromEntry(node, entry, where.command);
        where.placement = actions::OutputPlacement::Detached;
    }
    else
    {
        loadGuarded(child, "command", where.command);
        loadGuarded(child, "placement", where.placement);
    }

    // Validated once, here, so that the problem is reported when the file is read rather than at each click.
    // Kept as written: the opener disables it, and a save of the profile writes back what the user wrote.
    if (where.command.empty())
        return;
    if (auto const parsed = FileOpenerTemplate::parse(where.command); !parsed)
        errorLog()("file_opener '{}': {}; files open with the system's default application.",
                   where.command,
                   describe(parsed.error()));
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     PermissionsConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "capture_buffer", where.captureBuffer);
        loadFromEntry(child, "change_font", where.changeFont);
        loadFromEntry(child, "display_host_writable_statusline", where.displayHostWritableStatusLine);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, ColorConfig& where)
{
    if (auto const child = node[entry])
    {
        logger()("Loading entry: {}", entry);
        if (child.IsMap())
        {
            where = DualColorConfig { .colorSchemeLight = child["light"].as<std::string>(),
                                      .colorSchemeDark = child["dark"].as<std::string>() };
            loadFromEntry(doc["color_schemes"],
                          child["dark"].as<std::string>(),
                          std::get<DualColorConfig>(where).darkMode);

            loadFromEntry(doc["color_schemes"],
                          child["light"].as<std::string>(),
                          std::get<DualColorConfig>(where).lightMode);
        }
        else
        {
            where = SimpleColorConfig { .colorScheme = child.as<std::string>() };
            loadFromEntry(
                doc["color_schemes"], child.as<std::string>(), std::get<SimpleColorConfig>(where).colors);
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::TextShapingEngine& where)
{
    auto constexpr NativeTextShapingEngine =
#ifdef _WIN32
        vtrasterizer::TextShapingEngine::DWrite;
#elifdef __APPLE__
        vtrasterizer::TextShapingEngine::CoreText;
#else
        vtrasterizer::TextShapingEngine::OpenShaper;
#endif
    auto parseEngine = [NativeTextShapingEngine](
                           std::string_view literal) -> std::optional<vtrasterizer::TextShapingEngine> {
        using text::namesMatch;
        if (namesMatch(literal, "dwrite") || namesMatch(literal, "directwrite"))
            return vtrasterizer::TextShapingEngine::DWrite;
        if (namesMatch(literal, "core") || namesMatch(literal, "coretext"))
            return vtrasterizer::TextShapingEngine::CoreText;
        // `harfbuzz` is what std::formatter<TextShapingEngine> writes for OpenShaper, so it is the
        // spelling `contour generate config` puts in the file; without it the generated config does
        // not survive its own reader.
        if (namesMatch(literal, "open") || namesMatch(literal, "openshaper")
            || namesMatch(literal, "harfbuzz"))
            return vtrasterizer::TextShapingEngine::OpenShaper;
        if (namesMatch(literal, "native"))
            return NativeTextShapingEngine;
        return std::nullopt;
    };

    detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger, parseEngine);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::FontLocatorEngine& where)
{
    auto parseLocator = [](std::string_view literal) -> std::optional<vtrasterizer::FontLocatorEngine> {
        auto constexpr NativeFontLocator = vtrasterizer::FontLocatorEngine::Native;
        using text::namesMatch;
        for (auto const& deprecated: { "fontconfig", "coretext", "dwrite", "directwrite" })
        {
            if (namesMatch(literal, deprecated))
            {
                errorLog()(R"(Setting font locator to "{}" is deprecated. Use "native".)", literal);
                return NativeFontLocator;
            }
        }
        if (namesMatch(literal, "native"))
            return NativeFontLocator;
        if (namesMatch(literal, "mock"))
            return vtrasterizer::FontLocatorEngine::Mock;
        return std::nullopt;
    };

    detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger, parseLocator);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     text::RenderMode& where)
{
    detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger, text::makeRenderMode);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::FontDescriptions& where)
{
    if (auto const child = node[entry])
    {

        loadFromEntry(child, "size", where.size);
        loadFromEntry(child, "locator", where.fontLocator);
        // The engine lives under the nested `text_shaping:` map (as documented and generated), not a
        // literal flat "text_shaping.engine" key — a YAML node lookup by the dotted string never
        // matched, so a configured engine was silently ignored. Navigate the nested map.
        if (auto const textShaping = child["text_shaping"])
            loadFromEntry(textShaping, "engine", where.textShapingEngine);
        loadFromEntry(child, "builtin_box_drawing", where.builtinBoxDrawing);
        loadFromEntry(child, "max_fallback_count", where.maxFallbackCount);
        loadFromEntry(child, "render_mode", where.renderMode);
        loadFromEntry(child, "regular", where.regular);

        // inherit fonts from regular
        where.bold = where.regular;
        where.bold.weight = text::FontWeight::Bold;
        where.italic = where.regular;
        where.italic.slant = text::FontSlant::Italic;
        where.boldItalic = where.regular;
        where.boldItalic.slant = text::FontSlant::Italic;
        where.boldItalic.weight = text::FontWeight::Bold;

        loadFromEntry(child, "bold", where.bold);
        loadFromEntry(child, "italic", where.italic);
        loadFromEntry(child, "bold_italic", where.boldItalic);
        loadFromEntry(child, "emoji", where.emoji);

        // need separate loading since we need to save into font itself
        // TODO : must adhere to default behaviour from test_shaper/font
        bool strictSpacing = false;
        loadFromEntry(child, "strict_spacing", strictSpacing);
        where.regular.strictSpacing = strictSpacing;
        where.bold.strictSpacing = strictSpacing;
        where.italic.strictSpacing = strictSpacing;
        where.boldItalic.strictSpacing = strictSpacing;

        loadFromEntry(child, "text_outline", where.textOutline);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::TextOutlineConfig& where)
{
    if (auto const child = node[entry])
    {
        if (child.IsMap())
        {
            if (auto const thicknessNode = child["thickness"])
            {
                where.thickness = std::clamp(thicknessNode.as<float>(0.0f), 0.0f, 10.0f);
                logger()("Loading text_outline.thickness: {}", where.thickness);
            }
            if (auto const colorNode = child["color"])
            {
                auto const colorStr = colorNode.as<std::string>();
                where.color = vtbackend::RGBAColor(vtbackend::RGBColor(colorStr));
                logger()("Loading text_outline.color: {}", colorStr);
            }
        }
        else if (child.IsScalar())
        {
            // Simple form: text_outline: 1.0 (thickness only)
            where.thickness = std::clamp(child.as<float>(0.0f), 0.0f, 10.0f);
            logger()("Loading text_outline (scalar): {}", where.thickness);
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     ScrollBarPosition& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<ScrollBarPosition> {
        auto const literal = core::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "left")
            return ScrollBarPosition::Left;
        if (literal == "right")
            return ScrollBarPosition::Right;
        if (literal == "hidden")
            return ScrollBarPosition::Hidden;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, PixelReporting& where)
{
    // Case-insensitive. Unlike the other enum readers this reports an unrecognized value: the whole
    // point of the setting is to correct a visibly wrong image size, so a typo that silently leaves
    // the default would leave the user staring at the very symptom they were trying to fix.
    auto parseReporting = [&](std::string const& key) -> std::optional<PixelReporting> {
        auto const literal = core::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "logical")
            return PixelReporting::Logical;
        if (literal == "device")
            return PixelReporting::Device;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto const rawValue = child.as<std::string>();
        if (auto const opt = parseReporting(rawValue))
            where = opt.value();
        else
            errorLog()("Unknown pixel_reporting value '{}'; keeping {}.", rawValue, where);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::GlyphScalingMethod& where)
{
    // Case-insensitive, and an unrecognized value is reported rather than silently accepted -- a typo
    // in a visible rendering setting should not pass unnoticed. Mirrors the GuiTheme reader.
    if (auto const child = node[entry])
    {
        auto const rawValue = child.as<std::string>();
        if (auto const method = vtrasterizer::methodFromName(core::toLower(rawValue)))
            where = *method;
        else
            errorLog()("Invalid value for {}: '{}'. Expected 'stretch' or 'rerasterize'.", entry, rawValue);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, GuiTheme& where)
{
    // Case-insensitive. An unrecognized value is reported and leaves @p where at its (default)
    // value: a typo in a visible appearance setting should not silently pass unnoticed.
    auto parseTheme = [&](std::string const& key) -> std::optional<GuiTheme> {
        auto const literal = core::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "system")
            return GuiTheme::System;
        if (literal == "dark")
            return GuiTheme::Dark;
        if (literal == "light")
            return GuiTheme::Light;
        if (literal == "terminal")
            return GuiTheme::Terminal;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto const rawValue = child.as<std::string>();
        if (auto const opt = parseTheme(rawValue))
            where = opt.value();
        else
            errorLog()("Unknown theme value '{}'; keeping {}.", rawValue, where);
    }
}

namespace
{
    /// Reads a floor written as a whole number of seconds.
    ///
    /// A negative count reads as zero: "every command" is all a floor below zero could mean.
    /// @param node The value to read.
    /// @return The duration, or nullopt when @p node is not a whole number.
    [[nodiscard]] optional<chrono::seconds> wholeSecondsFloor(YAML::Node const& node)
    {
        if (!node.IsScalar())
            return nullopt;

        auto const text = string_view { node.Scalar() };
        auto const negative = text.starts_with('-');
        auto const count = core::toInteger<10, chrono::seconds::rep>(negative ? text.substr(1) : text);
        if (!count)
            return nullopt;
        return negative ? chrono::seconds::zero() : chrono::seconds { *count };
    }
} // namespace

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     DaemonRestoreConfig& where)
{
    // Only the children present are written, and each is read without as<>(), which would throw on a value
    // of the wrong kind and leave the keys after it unread.
    auto const child = node[entry];
    if (!child)
        return;

    loadGuarded(child, "scrollback", where.scrollback);

    if (auto const lines = child["scrollback_lines"])
    {
        auto const count = lines.IsScalar() ? core::toInteger<10, size_t>(lines.Scalar()) : nullopt;
        if (!count)
            errorLog()("{} must be a whole number of lines, not '{}'; keeping {}.",
                       keyPathTo(child, "scrollback_lines"),
                       YAML::Dump(lines),
                       where.scrollbackLines);
        else
        {
            if (*count > MaxRestoreScrollbackLines)
                errorLog()("{} is at most {}, not {}; using {}.",
                           keyPathTo(child, "scrollback_lines"),
                           MaxRestoreScrollbackLines,
                           *count,
                           MaxRestoreScrollbackLines);
            where.scrollbackLines = std::min(*count, MaxRestoreScrollbackLines);
        }
    }

    if (auto const programs = child["rerun_programs"])
    {
        if (!programs.IsSequence())
            errorLog()(
                "{} ({}) takes a list of program names, such as [vim, htop]; keeping the previous one.",
                keyPathTo(child, "rerun_programs"),
                detail::placeOf(programs));
        else
        {
            where.rerunPrograms.clear();
            for (auto const& program: programs)
            {
                if (program.IsScalar() && !program.Scalar().empty())
                    where.rerunPrograms.push_back(program.Scalar());
                else
                    errorLog()("{} ({}) lists something other than a program name; ignored.",
                               keyPathTo(child, "rerun_programs"),
                               detail::placeOf(program));
            }
        }
    }

    if (auto const interval = child["interval"])
    {
        if (auto const parsed = wholeSecondsFloor(interval); !parsed)
            errorLog()("{} must be a whole number of seconds, not '{}'; keeping {}.",
                       keyPathTo(child, "interval"),
                       YAML::Dump(interval),
                       where.interval.count());
        else
        {
            if (*parsed < MinRestoreInterval)
                errorLog()("{} is at least {} s, not {} s; using {} s.",
                           keyPathTo(child, "interval"),
                           MinRestoreInterval.count(),
                           parsed->count(),
                           MinRestoreInterval.count());
            where.interval = std::max(*parsed, MinRestoreInterval);
        }
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     FinishNotificationConfig& where)
{
    // Only the children present are written: an override of one key must leave its siblings as they were.
    auto const child = node[entry];
    if (!child)
        return;

    loadFromEntry(child, "when", where.when);
    // Not read with as<int>(): a value that is no number would throw, and the keys after it would go unread.
    if (auto const minDuration = child["min_duration"])
    {
        if (auto const parsed = wholeSecondsFloor(minDuration))
            where.minDuration = *parsed;
        else
            errorLog()("{}.min_duration must be a whole number of seconds, not '{}'; keeping {}.",
                       entry,
                       YAML::Dump(minDuration),
                       where.minDuration.count());
    }
    loadFromEntry(child, "outcome", where.outcome);
    loadFromEntry(child, "action", where.action);
    loadFromEntry(child, "clear_on", where.clearOn);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::StatusDisplayPosition& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::StatusDisplayPosition> {
        auto const literal = core::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "bottom")
            return vtbackend::StatusDisplayPosition::Bottom;
        if (literal == "top")
            return vtbackend::StatusDisplayPosition::Top;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, std::string& where)
{
    if (auto const child = node[entry])
    {
        where = child.as<std::string>();
        logger()("Loading entry: {}, value {}", entry, where);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::ImageSize& where)
{

    if (auto const child = node[entry])
    {
        loadFromEntry(child, "max_width", where.width);
        loadFromEntry(child, "max_height", where.height);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, InputMappings& where)
{

    if (auto const child = node[entry])
    {

        // Defining `input_mapping` REPLACES the built-in bindings rather than adding to them: the
        // section is a declaration that the user is defining the full set. This is intended and
        // documented (docs/configuration/key-mapping.md); the config Contour generates on first run
        // writes every default out in full, so editing that section keeps the untouched ones.
        where = {};
        if (child.IsSequence())
        {
            for (auto&& mapping: child)
            {
                auto action = parseAction(mapping);
                auto mods = parseModifier(mapping);
                auto mode = parseMatchModes(mapping);
                auto const clicks = parseClickCount(mapping);

                // A count belongs to a button the click counter counts: a key has no clicks, and every
                // step of a wheel is a first one. Bound anyway, the count would never come -- or, on a row
                // that also names a key, be dropped in silence.
                auto const countsClicks = [&]() {
                    auto const button = parseMouseButton(mapping["mouse"]);
                    return !mapping["key"] && button && vtbackend::ClickCounter::countsClicks(*button);
                };

                if (action && mods && mode && clicks)
                {
                    if (clicks->has_value() && !countsClicks())
                    {
                        errorLog()("Dropping input_mapping entry [{}]: 'clicks' applies to the {} mouse "
                                   "buttons only.",
                                   describeInputMappingRow(mapping),
                                   countedButtonNames());
                    }
                    else if (tryAddKey(where, *mode, *mods, mapping["key"], *action))
                    {
                        logger()("Adding input mapping: mods: {:<20} modifiers: {:<20} key: {:<20} "
                                 "action: {:<20}",
                                 *mods,
                                 *mode,
                                 mapping["key"].as<std::string>(),
                                 *action);
                    }
                    else if (tryAddMouse(
                                 where.mouseMappings, *mode, *mods, mapping["mouse"], *action, *clicks))
                    {
                        logger()("Adding input mapping: mods: {:<20} modifiers: {:<20} mouse: {:<18} clicks: "
                                 "{:<3} action: {:<20}",
                                 *mods,
                                 *mode,
                                 mapping["mouse"].as<std::string>(),
                                 clicks->has_value() ? std::format("{}", **clicks) : std::string { "-" },
                                 *action);
                    }
                    else
                    {
                        errorLog()("Dropping input_mapping entry [{}]: it names neither a bindable "
                                   "'key' nor a 'mouse' button.",
                                   describeInputMappingRow(mapping));
                    }
                }
                else
                {
                    // Say which row died and which field killed it. Before this, one misspelled
                    // modifier made an entire binding vanish with no output at all -- see issue
                    // #1987, where `mods: [Shift,Alt,Ctrl]` produced nothing but silence.
                    auto unparsed = std::vector<std::string_view> {};
                    if (!action)
                        unparsed.emplace_back("action");
                    if (!mods)
                        unparsed.emplace_back("mods");
                    if (!mode)
                        unparsed.emplace_back("mode");
                    if (!clicks)
                        unparsed.emplace_back(clicks.error());
                    errorLog()("Dropping input_mapping entry [{}]: could not parse its {}.",
                               describeInputMappingRow(mapping),
                               unparsed | core::views::joinWith(", "));
                }
            }
        }
        else
        {
            // The section is present but is not a list, so not one binding can be read out of it --
            // and the wipe above has already happened. Without this the user is left with a keyboard
            // that has nothing bound and no explanation, which is the silent-loss failure of issue
            // #1987 over again, only total rather than per-row.
            errorLog()("Ignoring `input_mapping`: expected a list of bindings. The built-in key "
                       "bindings have been discarded, so no key binding is currently active.");
        }
    }
}
void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::BoxDrawingRenderer::ArcStyle& where)
{
    using ArcStyle = vtrasterizer::BoxDrawingRenderer::ArcStyle;
    if (auto const child = node[entry])
    {
        auto const styleName = core::toLower(child.as<std::string>());
        auto constexpr static Mappings = std::array {
            std::pair { "", ArcStyle::Round },
            std::pair { "round", ArcStyle::Round },
            std::pair { "elliptic", ArcStyle::Elliptic },
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == styleName)
            {
                logger()("Loading entry: arc_style, value {}", styleName);
                where = mapping.second;
                return;
            }
    }
}
void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle& where)
{
    where = {};
    using BranchStyle = vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle::BranchStyle;
    using MergeCommitStyle = vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle::MergeCommitStyle;

    auto const parseBranchStyle = [&](std::string const& style) -> std::optional<BranchStyle> {
        auto const styleName = core::toLower(style);
        auto constexpr static Mappings = std::array {
            std::pair { "", BranchStyle::None },         //
            std::pair { "none", BranchStyle::None },     //
            std::pair { "thin", BranchStyle::Thin },     //
            std::pair { "double", BranchStyle::Double }, //
            std::pair { "thick", BranchStyle::Thick },
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == styleName)
            {
                logger()("Loading entry: branch_style, value {}", styleName);
                return mapping.second;
            }
        return std::nullopt;
    };

    auto const parseMCStyle = [&](std::string const& style) -> std::optional<MergeCommitStyle> {
        auto const styleName = core::toLower(style);
        auto constexpr static Mappings = std::array {
            std::pair { "", MergeCommitStyle::Bullet },       //
            std::pair { "bullet", MergeCommitStyle::Bullet }, //
            std::pair { "solid", MergeCommitStyle::Solid },   //
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == styleName)
            {
                logger()("Loading entry: merge_commit_style, value {}", styleName);
                return mapping.second;
            }
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        loadFromEntry(child, "arc_style", where.arcStyle);
        if (auto const branchStyle = parseBranchStyle(child["branch_style"].as<std::string>()))
            where.branchStyle = *branchStyle;
        if (auto const mcStyle = parseMCStyle(child["merge_commit_style"].as<std::string>()))
            where.mergeCommitStyle = *mcStyle;
    }
}
void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::BoxDrawingRenderer::BrailleStyle& where)
{
    using BrailleStyle = vtrasterizer::BoxDrawingRenderer::BrailleStyle;
    if (auto const child = node[entry])
    {
        auto const styleName = core::toLower(child.as<std::string>());
        auto constexpr static Mappings = std::array {
            std::pair { "font", BrailleStyle::Font },                     //
            std::pair { "solid", BrailleStyle::Solid },                   //
            std::pair { "", BrailleStyle::Circle },                       //
            std::pair { "circle", BrailleStyle::Circle },                 //
            std::pair { "circle_empty", BrailleStyle::CircleEmpty },      //
            std::pair { "square", BrailleStyle::SquareEmpty },            //
            std::pair { "square_empty", BrailleStyle::SquareEmpty },      //
            std::pair { "aa_square", BrailleStyle::AASquare },            //
            std::pair { "aa_square_empty", BrailleStyle::AASquareEmpty }, //
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == styleName)
            {
                logger()("Loading entry: {}, value {}", entry, styleName);
                where = mapping.second;
                return;
            }
    }
}

bool YAMLConfigReader::tryAddMouse(std::vector<MouseInputMapping>& bindings,
                                   vtbackend::MatchModes modes,
                                   vtbackend::Modifiers modifier,
                                   YAML::Node const& node,
                                   actions::Action action,
                                   std::optional<uint8_t> clickCount)
{
    auto mouseButton = parseMouseButton(node);
    if (!mouseButton)
        return false;

    appendOrCreateBinding(bindings, modes, modifier, *mouseButton, std::move(action), clickCount);
    return true;
}

void YAMLConfigReader::defaultSettings(vtpty::Process::ExecInfo& shell)
{
    shell.env["TERMINAL_NAME"] = "contour";
    shell.env["TERMINAL_VERSION_TRIPLE"] =
        std::format("{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH);
    shell.env["TERMINAL_VERSION_STRING"] = CONTOUR_VERSION_STRING;

    // {{{ Populate environment variables
    std::optional<fs::path> appTerminfoDir; // NOLINT(misc-const-correctness)
#ifdef __APPLE__
    {
        char buf[1024];
        uint32_t len = sizeof(buf);
        if (_NSGetExecutablePath(buf, &len) == 0)
        {
            auto p = fs::path(buf).parent_path().parent_path() / "Resources" / "terminfo";
            if (fs::is_directory(p))
            {
                appTerminfoDir = p;
                shell.env["TERMINFO_DIRS"] = p.string();
            }
        }
    }
#endif

    // force some default env
    if (!shell.env.contains("TERM"))
    {
        shell.env["TERM"] = getDefaultTERM(appTerminfoDir);
        logger()("Defaulting TERM to {}.", shell.env["TERM"]);
    }

    if (!shell.env.contains("COLORTERM"))
        shell.env["COLORTERM"] = "truecolor";

    // TERM_PROGRAM / TERM_PROGRAM_VERSION are the de-facto way an application identifies WHICH
    // terminal it is talking to, as opposed to TERM, which only names a terminfo capability set --
    // several terminals share `xterm-256color`, so TERM cannot distinguish them.
    //
    // This matters beyond cosmetics: Python wcwidth's wcstwidth() selects its per-terminal character
    // width correction table by TERM_PROGRAM. Without it Contour is simply invisible to that
    // ecosystem, and applications relying on it fall back to a generic measurement.
    //
    // Set unconditionally rather than only-if-absent: an inherited value would name the OUTER
    // terminal, and answering "I am the terminal you are not talking to" is worse than not answering.
    shell.env["TERM_PROGRAM"] = "contour";
    shell.env["TERM_PROGRAM_VERSION"] = CONTOUR_VERSION_STRING;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::optional<vtbackend::MouseButton> YAMLConfigReader::parseMouseButton(YAML::Node const& node)
{
    using namespace std::literals::string_view_literals;
    if (!node)
        return std::nullopt;

    if (!node.IsScalar())
        return std::nullopt;

    auto constexpr static Mappings = std::array {
        std::pair { "WHEELUP"sv, vtbackend::MouseButton::WheelUp },
        std::pair { "WHEELDOWN"sv, vtbackend::MouseButton::WheelDown },
        std::pair { "WHEELLEFT"sv, vtbackend::MouseButton::WheelLeft },
        std::pair { "WHEELRIGHT"sv, vtbackend::MouseButton::WheelRight },
        std::pair { "LEFT"sv, vtbackend::MouseButton::Left },
        std::pair { "MIDDLE"sv, vtbackend::MouseButton::Middle },
        std::pair { "RIGHT"sv, vtbackend::MouseButton::Right },
    };
    auto const upperName = core::toUpper(node.as<std::string>());
    for (auto const& mapping: Mappings)
        if (upperName == mapping.first)
            return mapping.second;
    return std::nullopt;
}

bool YAMLConfigReader::tryAddKey(InputMappings& inputMappings,
                                 vtbackend::MatchModes modes,
                                 vtbackend::Modifiers modifier,
                                 YAML::Node const& node,
                                 actions::Action action)
{
    if (!node)
        return false;

    if (!node.IsScalar())
        return false;

    auto const input = parseKeyOrChar(node.as<std::string>());
    if (!input.has_value())
    {
        logger()("Could not parse key:{}", node.as<std::string>());
        return false;
    }

    if (holds_alternative<vtbackend::Key>(*input))
    {
        appendOrCreateBinding(
            inputMappings.keyMappings, modes, modifier, get<vtbackend::Key>(*input), std::move(action));
    }
    else if (holds_alternative<char32_t>(*input))
    {
        appendOrCreateBinding(
            inputMappings.charMappings, modes, modifier, get<char32_t>(*input), std::move(action));
    }
    else
        assert(false && "The impossible happened.");

    return true;
}

std::optional<std::variant<vtbackend::Key, char32_t>> YAMLConfigReader::parseKeyOrChar(
    std::string const& name)
{
    using namespace vtbackend::ControlCode;
    using namespace std::literals::string_view_literals;

    if (auto const key = parseKey(name); key.has_value())
        return key.value();

    auto const text = unicode::convert_to<char32_t>(std::string_view { name });
    if (text.size() == 1)
        // Folded, because the case a letter arrives in is decided by the input route rather than by
        // the user; the lookup in TerminalSession::sendCharEvent folds the delivered codepoint to
        // match. Both sides must agree -- folding only here would break `key: 'a'` bindings that
        // work today. @see config::foldedBindingCodepoint
        return foldedBindingCodepoint(static_cast<char32_t>(text[0]));

    auto constexpr NamedChars =
        std::array { std::pair { "LESS"sv, '<' },          std::pair { "GREATER"sv, '>' },
                     std::pair { "PLUS"sv, '+' },          std::pair { "APOSTROPHE"sv, '\'' },
                     std::pair { "ADD"sv, '+' },           std::pair { "BACKSLASH"sv, '\\' },
                     std::pair { "COMMA"sv, ',' },         std::pair { "DECIMAL"sv, '.' },
                     std::pair { "DIVIDE"sv, '/' },        std::pair { "EQUAL"sv, '=' },
                     std::pair { "LEFT_BRACKET"sv, '[' },  std::pair { "MINUS"sv, '-' },
                     std::pair { "MULTIPLY"sv, '*' },      std::pair { "PERIOD"sv, '.' },
                     std::pair { "RIGHT_BRACKET"sv, ']' }, std::pair { "SEMICOLON"sv, ';' },
                     std::pair { "SLASH"sv, '/' },         std::pair { "SUBTRACT"sv, '-' },
                     std::pair { "SPACE"sv, ' ' } };

    auto const upperName = core::toUpper(name);
    for (auto const& mapping: NamedChars)
        if (upperName == mapping.first)
            return static_cast<char32_t>(mapping.second);

    return std::nullopt;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::optional<vtbackend::Key> YAMLConfigReader::parseKey(std::string const& name)
{
    using vtbackend::Key;
    using namespace std::literals::string_view_literals;
    auto static constexpr Mappings =
        std::array { std::pair { "F1"sv, Key::F1 },
                     std::pair { "F2"sv, Key::F2 },
                     std::pair { "F3"sv, Key::F3 },
                     std::pair { "F4"sv, Key::F4 },
                     std::pair { "F5"sv, Key::F5 },
                     std::pair { "F6"sv, Key::F6 },
                     std::pair { "F7"sv, Key::F7 },
                     std::pair { "F8"sv, Key::F8 },
                     std::pair { "F9"sv, Key::F9 },
                     std::pair { "F10"sv, Key::F10 },
                     std::pair { "F11"sv, Key::F11 },
                     std::pair { "F12"sv, Key::F12 },
                     std::pair { "F13"sv, Key::F13 },
                     std::pair { "F14"sv, Key::F14 },
                     std::pair { "F15"sv, Key::F15 },
                     std::pair { "F16"sv, Key::F16 },
                     std::pair { "F17"sv, Key::F17 },
                     std::pair { "F18"sv, Key::F18 },
                     std::pair { "F19"sv, Key::F19 },
                     std::pair { "F20"sv, Key::F20 },
                     std::pair { "F21"sv, Key::F21 },
                     std::pair { "F22"sv, Key::F22 },
                     std::pair { "F23"sv, Key::F23 },
                     std::pair { "F24"sv, Key::F24 },
                     std::pair { "F25"sv, Key::F25 },
                     std::pair { "F26"sv, Key::F26 },
                     std::pair { "F27"sv, Key::F27 },
                     std::pair { "F28"sv, Key::F28 },
                     std::pair { "F29"sv, Key::F29 },
                     std::pair { "F30"sv, Key::F30 },
                     std::pair { "F31"sv, Key::F31 },
                     std::pair { "F32"sv, Key::F32 },
                     std::pair { "F33"sv, Key::F33 },
                     std::pair { "F34"sv, Key::F34 },
                     std::pair { "F35"sv, Key::F35 },
                     std::pair { "Escape"sv, Key::Escape },
                     std::pair { "Enter"sv, Key::Enter },
                     std::pair { "Tab"sv, Key::Tab },
                     std::pair { "Backspace"sv, Key::Backspace },
                     std::pair { "DownArrow"sv, Key::DownArrow },
                     std::pair { "LeftArrow"sv, Key::LeftArrow },
                     std::pair { "RightArrow"sv, Key::RightArrow },
                     std::pair { "UpArrow"sv, Key::UpArrow },
                     std::pair { "Insert"sv, Key::Insert },
                     std::pair { "Delete"sv, Key::Delete },
                     std::pair { "Home"sv, Key::Home },
                     std::pair { "End"sv, Key::End },
                     std::pair { "PageUp"sv, Key::PageUp },
                     std::pair { "PageDown"sv, Key::PageDown },
                     std::pair { "MediaPlay"sv, Key::MediaPlay },
                     std::pair { "MediaStop"sv, Key::MediaStop },
                     std::pair { "MediaPrevious"sv, Key::MediaPrevious },
                     std::pair { "MediaNext"sv, Key::MediaNext },
                     std::pair { "MediaPause"sv, Key::MediaPause },
                     std::pair { "MediaTogglePlayPause"sv, Key::MediaTogglePlayPause },
                     std::pair { "VolumeUp"sv, Key::VolumeUp },
                     std::pair { "VolumeDown"sv, Key::VolumeDown },
                     std::pair { "VolumeMute"sv, Key::VolumeMute },
                     std::pair { "PrintScreen"sv, Key::PrintScreen },
                     std::pair { "Pause"sv, Key::Pause },
                     std::pair { "Menu"sv, Key::Menu },
                     std::pair { "Numpad_Add"sv, Key::Numpad_Add },
                     std::pair { "Numpad_Divide"sv, Key::Numpad_Divide },
                     std::pair { "Numpad_Multiply"sv, Key::Numpad_Multiply },
                     std::pair { "Numpad_Subtract"sv, Key::Numpad_Subtract },
                     std::pair { "Numpad_Decimal"sv, Key::Numpad_Decimal },
                     std::pair { "Numpad_Enter"sv, Key::Numpad_Enter },
                     std::pair { "Numpad_Equal"sv, Key::Numpad_Equal },
                     std::pair { "Numpad_0"sv, Key::Numpad_0 },
                     std::pair { "Numpad_1"sv, Key::Numpad_1 },
                     std::pair { "Numpad_2"sv, Key::Numpad_2 },
                     std::pair { "Numpad_3"sv, Key::Numpad_3 },
                     std::pair { "Numpad_4"sv, Key::Numpad_4 },
                     std::pair { "Numpad_5"sv, Key::Numpad_5 },
                     std::pair { "Numpad_6"sv, Key::Numpad_6 },
                     std::pair { "Numpad_7"sv, Key::Numpad_7 },
                     std::pair { "Numpad_8"sv, Key::Numpad_8 },
                     std::pair { "Numpad_9"sv, Key::Numpad_9 } };

    auto const lowerName = core::toLower(name);

    for (auto const& mapping: Mappings)
        if (lowerName == core::toLower(mapping.first))
            return mapping.second;

    return std::nullopt;
}

std::optional<vtbackend::MatchModes> YAMLConfigReader::parseMatchModes(YAML::Node const& nodeYAML)
{
    using vtbackend::MatchModes;

    auto node = nodeYAML["mode"];
    if (!node)
        return MatchModes {};
    if (!node.IsScalar())
        return std::nullopt;

    auto matchModes = MatchModes {};

    auto const modeStr = node.as<std::string>();
    auto const args = core::split(modeStr, '|');
    for (std::string_view arg: args)
    {
        if (arg.empty())
            continue;
        bool negate = false;
        if (arg.front() == '~')
        {
            negate = true;
            arg.remove_prefix(1);
        }

        MatchModes::Flag flag = MatchModes::Flag::Default;
        std::string const upperArg = core::toUpper(arg);
        if (upperArg == "ALT")
            flag = MatchModes::AlternateScreen;
        else if (upperArg == "ALTSCREEN")
            flag = MatchModes::AlternateScreen;
        else if (upperArg == "APPCURSOR")
            flag = MatchModes::AppCursor;
        else if (upperArg == "APPKEYPAD")
            flag = MatchModes::AppKeypad;
        else if (upperArg == "INSERT")
            flag = MatchModes::Insert;
        else if (upperArg == "SELECT")
            flag = MatchModes::Select;
        else if (upperArg == "SEARCH")
            flag = MatchModes::Search;
        else if (upperArg == "TRACE")
            flag = MatchModes::Trace;
        else
        {
            errorLog()("Unknown input_mapping mode: {}", arg);
            continue;
        }

        if (negate)
            matchModes.disable(flag);
        else
            matchModes.enable(flag);
    }

    return matchModes;
}

std::optional<vtbackend::Modifiers> YAMLConfigReader::parseModifier(YAML::Node const& nodeYAML)
{
    using vtbackend::Modifier;
    auto node = nodeYAML["mods"];
    if (!node)
        return std::nullopt;
    if (node.IsScalar())
        return parseModifierKey(node.as<std::string>());
    if (!node.IsSequence())
        return std::nullopt;

    vtbackend::Modifiers mods;
    for (auto const& i: node)
    {
        if (!i.IsScalar())
            return std::nullopt;

        auto const mod = parseModifierKey(i.as<std::string>());
        if (!mod)
            return std::nullopt;

        mods |= *mod;
    }
    return mods;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::optional<vtbackend::Modifiers> YAMLConfigReader::parseModifierKey(std::string const& key)
{
    if (auto const modifier = parseModifierName(key))
        return vtbackend::Modifiers { *modifier };

    // Report rather than return a bare nullopt: an unaccepted spelling takes the whole binding down
    // with it (parseModifier bails on the first bad element), and before this said so out loud a
    // `mods: [Ctrl]` typo made the entire row vanish in silence -- see issue #1987.
    errorLog()("Unknown modifier '{}'; expected one of: {}.", key, acceptedModifierSpellings());
    return std::nullopt;
}

namespace
{
    /// The argument @p key of an action, read from its scalar text by @p parse.
    ///
    /// A present argument that does not parse drops the binding instead of falling back: a typo must not
    /// quietly turn the action into a different one.
    /// @param node The action's YAML map.
    /// @param key The argument's key.
    /// @param fallback What an absent key means.
    /// @param parse Maps the argument's text to a value, or to nullopt when it names none.
    /// @return The value, or nullopt (logged) when the key is present but is no scalar @p parse accepts.
    template <typename T, typename Parser>
    [[nodiscard]] std::optional<T> actionArgument(YAML::Node const& node,
                                                  char const* key,
                                                  T fallback,
                                                  Parser parse)
    {
        auto const value = node[key];
        if (!value)
            return fallback;
        auto const token = value.IsScalar() ? value.as<std::string>() : std::string {};
        if (auto const parsed = parse(token))
            return parsed;
        errorLog()("Invalid {} '{}' in action '{}'; ignoring the binding.",
                   key,
                   token,
                   node["action"].as<std::string>());
        return std::nullopt;
    }

    /// The enum-valued argument @p key of an action, read through its ConfigEnum token table.
    /// @param node The action's YAML map.
    /// @param key The argument's key.
    /// @param fallback What an absent key means.
    /// @return The value, or nullopt (logged) when the key names no value of @p Enum.
    template <typename Enum>
    [[nodiscard]] std::optional<Enum> enumActionArgument(YAML::Node const& node,
                                                         char const* key,
                                                         Enum fallback)
    {
        return actionArgument(node, key, fallback, configEnumFromToken<Enum>);
    }

    /// A command block id written as decimal digits, and nothing else: no sign, no other base.
    /// @param text The argument's text.
    /// @return The id, or nullopt when @p text is not such a number or lies past the id's range.
    [[nodiscard]] std::optional<vtbackend::CommandBlockId> parseCommandBlockId(std::string_view text) noexcept
    {
        return core::toInteger<10, uint32_t>(text).transform(
            [](uint32_t value) { return vtbackend::CommandBlockId(value); });
    }

    /// Whether an action's own arguments all named a value.
    enum class ArgumentsRead : uint8_t
    {
        Valid = 0, ///< Each was absent or named a value.
        Invalid,   ///< At least one named none, and was logged.
    };

    /// Reads the arguments a block action has beside the ones that address its block: none, for most.
    /// @return Valid.
    template <actions::CommandBlockActionConcept BlockAction>
    [[nodiscard]] ArgumentsRead readOwnArguments(YAML::Node const& /*node*/, BlockAction& /*action*/)
    {
        return ArgumentsRead::Valid;
    }

    /// Reads OpenCommandOutput's program, placement and format into @p action. An empty program, or one
    /// that is no scalar, is no program: running the configured pager instead would be a guess. An absent
    /// format stays absent, so the program's own format applies (see outputFormatFor).
    /// @param node The action's YAML map.
    /// @param action The action, with its defaults; receives every argument that named a value.
    /// @return Invalid (logged) when one of them named none.
    [[nodiscard]] ArgumentsRead readOwnArguments(YAML::Node const& node, actions::OpenCommandOutput& action)
    {
        auto const placement = enumActionArgument(node, "placement", action.placement);
        auto const format = actionArgument(node, "format", action.format, [](std::string const& text) {
            return configEnumFromToken<actions::OutputFormat>(text).transform(
                [](actions::OutputFormat value) { return std::optional { value }; });
        });
        auto program = actionArgument(node, "program", action.program, [](std::string const& text) {
            return text.empty() ? std::nullopt : std::optional { text };
        });
        if (!placement || !format || !program)
            return ArgumentsRead::Invalid;
        action.placement = *placement;
        action.format = *format;
        action.program = std::move(*program);
        return ArgumentsRead::Valid;
    }

    /// The arguments of a block action: `target:`, `part:` where it has a part, the pinning `block:`, and
    /// the action's own, each defaulting to what @p action already holds.
    /// @param node The action's YAML map.
    /// @param action The action, with its defaults.
    /// @return The action with its arguments, or nullopt (logged) when one of them names no value.
    template <actions::CommandBlockActionConcept BlockAction>
    [[nodiscard]] std::optional<BlockAction> commandBlockAction(YAML::Node const& node, BlockAction action)
    {
        // Every argument is read before any is judged, so each misspelt one is logged.
        auto const target = enumActionArgument(node, "target", action.target);
        auto const block = actionArgument(node, "block", action.block, parseCommandBlockId);
        auto const own = readOwnArguments(node, action);
        if constexpr (requires { action.part; })
        {
            auto const part = enumActionArgument(node, "part", action.part);
            if (!part)
                return std::nullopt;
            action.part = *part;
        }
        if (!target || !block || own != ArgumentsRead::Valid)
            return std::nullopt;
        action.target = *target;
        action.block = *block;
        return action;
    }

    /// Reads the arguments of a block action, for any action.
    /// @see commandBlockAction
    /// @param node The action's YAML map.
    /// @param action The action, with its defaults.
    /// @return The action with its arguments, or nullopt (logged) when one of them names no value. Any
    ///         other action comes back unchanged: it addresses no block.
    [[nodiscard]] std::optional<actions::Action> withCommandBlockArguments(YAML::Node const& node,
                                                                           actions::Action const& action)
    {
        return std::visit(core::Overloaded {
                              [&](actions::CommandBlockActionConcept auto const& blockAction) {
                                  return commandBlockAction(node, blockAction).transform([](auto read) {
                                      return actions::Action { std::move(read) };
                                  });
                              },
                              [&](auto const&) { return std::optional { action }; },
                          },
                          action);
    }
} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<actions::Action> YAMLConfigReader::parseAction(YAML::Node const& node)
{
    if (auto actionNode = node["action"])
    {
        auto actionName = actionNode.as<std::string>();
        auto actionOpt = actions::fromString(actionName);
        if (!actionOpt)
        {
            errorLog()("Unknown action '{}'.", actionName);
            return std::nullopt;
        }
        auto action = actionOpt.value();
        if (holds_alternative<actions::ChangeProfile>(action))
        {
            if (auto name = node["name"]; name && name.IsScalar())
            {
                return actions::ChangeProfile { actionNode.as<std::string>() };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::CreateNewTab>(action))
        {
            // Optional: a bare `action: CreateNewTab` opens the default profile, which is what the
            // shipped binding does. A `profile:` sibling names one instead.
            if (auto profile = node["profile"]; profile && profile.IsScalar())
                return actions::CreateNewTab { profile.as<std::string>() };
            return actions::CreateNewTab {};
        }

        if (holds_alternative<actions::SetTabBarVisibility>(action))
        {
            if (auto mode = node["mode"]; mode && mode.IsScalar())
                if (auto const parsed = configEnumFromToken<TabBarVisibility>(mode.as<std::string>()))
                    return actions::SetTabBarVisibility { *parsed };
            return std::nullopt;
        }

        if (holds_alternative<actions::SetTabBarPosition>(action))
        {
            if (auto position = node["position"]; position && position.IsScalar())
                if (auto const parsed = configEnumFromToken<TabBarPosition>(position.as<std::string>()))
                    return actions::SetTabBarPosition { *parsed };
            return std::nullopt;
        }

        if (holds_alternative<actions::MoveTabTo>(action))
        {
            if (auto position = node["position"]; position && position.IsScalar())
                return actions::MoveTabTo { position.as<int>() };
            else
                return std::nullopt;
        }

        // ToggleFoldAt is built by the context menu from the line that was right-clicked rather than
        // bound by hand, but it round-trips like any other parameterized action -- a config written by
        // Contour must be a config Contour can read back.
        if (holds_alternative<actions::ToggleFoldAt>(action))
        {
            if (auto line = node["line"]; line && line.IsScalar())
                return actions::ToggleFoldAt { line.as<int>() };
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::SwitchToTab>(action))
        {
            if (auto position = node["position"]; position && position.IsScalar())
            {
                return actions::SwitchToTab { position.as<int>() };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::SetTabColor>(action))
        {
            // `color` is OPTIONAL: without it the action opens the tab's color picker, which is a
            // perfectly good thing to bind a key to. A color that does not parse is therefore not fatal —
            // like CopySelection's bad `format`, it warns and falls back to the default (the picker)
            // rather than dropping the whole binding on the floor.
            if (auto const colorNode = node["color"]; colorNode)
            {
                // A color key that YAML did not hand us as a scalar is the unquoted `color: #ff0000`,
                // whose `#` starts a COMMENT — leaving a null node here and the user with a key that
                // silently opens the picker instead of coloring the tab. Warn: the value is gone by the
                // time it reaches us, so this diagnostic is the only thing pointing at the quoting.
                if (!colorNode.IsScalar())
                {
                    logger()("Non-scalar color in SetTabColor action (an unquoted '#rrggbb' is a YAML "
                             "comment — quote it). Falling back to opening the color picker.");
                    return action;
                }

                auto const colorText = colorNode.as<std::string>();
                if (auto const color = vtbackend::parseColor(colorText); color.has_value())
                    return actions::SetTabColor { color };

                logger()(
                    "Invalid color '{}' in SetTabColor action. Falling back to opening the color picker.",
                    colorText);
            }
            return action; // colorless => the picker
        }

        if (holds_alternative<actions::ResizePane>(action))
        {
            auto const directionNode = node["direction"];
            if (!directionNode || !directionNode.IsScalar())
                return std::nullopt; // direction is required
            auto const parsed = parseResizeDirection(directionNode.as<std::string>());
            if (!parsed.has_value())
                return std::nullopt; // unknown direction string
            auto const percentNode = node["percent"];
            auto const percent = (percentNode && percentNode.IsScalar()) ? percentNode.as<int>() : 5;
            return actions::ResizePane { *parsed, percent };
        }

        if (holds_alternative<actions::NewTerminal>(action))
        {
            if (auto profile = node["profile"]; profile && profile.IsScalar())
            {
                return actions::NewTerminal { profile.as<std::string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::ReloadConfig>(action))
        {
            if (auto profileName = node["profile"]; profileName && profileName.IsScalar())
            {
                return actions::ReloadConfig { profileName.as<std::string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::SendChars>(action))
        {
            if (auto chars = node["chars"]; chars && chars.IsScalar())
            {
                return actions::SendChars { core::unescape(chars.as<std::string>()) };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::CopySelection>(action))
        {
            // Unlike a misspelt format, which has always meant text, a misspelt fallback drops the binding:
            // guessing would either empty the clipboard or fill it with output the user did not ask for.
            auto const defaults = actions::CopySelection {};
            auto const fallback = enumActionArgument(node, "fallback", defaults.fallback);
            if (!fallback)
                return std::nullopt;

            auto format = defaults.format;
            if (auto nodeFormat = node["format"]; nodeFormat && nodeFormat.IsScalar())
            {
                if (auto const parsed =
                        configEnumFromToken<actions::CopyFormat>(nodeFormat.as<std::string>()))
                    format = *parsed;
                else
                    logger()("Invalid format '{}' in CopySelection action. Defaulting to 'text'.",
                             nodeFormat.as<std::string>());
            }
            return actions::CopySelection { .format = format, .fallback = *fallback };
        }

        if (holds_alternative<actions::PasteClipboard>(action))
        {
            if (auto nodeStrip = node["strip"]; nodeStrip && nodeStrip.IsScalar())
            {
                return actions::PasteClipboard { nodeStrip.as<bool>() };
            }
        }

        if (holds_alternative<actions::OpenConfiguration>(action))
        {
            // Default opens the in-app settings page; `in_editor: true` opens the config file instead.
            if (auto inEditor = node["in_editor"]; inEditor && inEditor.IsScalar())
                return actions::OpenConfiguration { inEditor.as<bool>() };
            return action;
        }

        if (holds_alternative<actions::PasteSelection>(action))
        {
            if (auto eval = node["evaluate_in_shell"]; eval && eval.IsScalar())
            {
                return actions::PasteSelection { eval.as<bool>() };
            }
        }

        if (holds_alternative<actions::WriteScreen>(action))
        {
            if (auto chars = node["chars"]; chars && chars.IsScalar())
            {
                return actions::WriteScreen { chars.as<std::string>() };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::LaunchLayout>(action))
        {
            if (auto name = node["name"]; name && name.IsScalar())
                return actions::LaunchLayout { name.as<std::string>() };
            return std::nullopt;
        }

        if (holds_alternative<actions::AttachToHost>(action))
        {
            // Like LaunchLayout: a nameless "attach to which host?" has no default, so it is dropped.
            if (auto name = node["name"]; name && name.IsScalar())
                return actions::AttachToHost { name.as<std::string>() };
            return std::nullopt;
        }

        if (holds_alternative<actions::SaveLayout>(action))
        {
            // The name is OPTIONAL, unlike LaunchLayout above: with one, SaveLayout saves straight to it;
            // without, it opens the save-as prompt (the mirror of a colorless SetTabColor). So a nameless
            // binding is kept, not dropped.
            if (auto name = node["name"]; name && name.IsScalar())
                return actions::SaveLayout { name.as<std::string>() };
            return actions::SaveLayout {};
        }

        if (holds_alternative<actions::CreateSelection>(action))
        {
            if (auto delimiters = node["delimiters"]; delimiters && delimiters.IsScalar())
            {
                return actions::CreateSelection { delimiters.as<std::string>() };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::HintMode>(action))
        {
            auto hintAction = vtbackend::HintAction::Copy;
            if (auto nodeHintAction = node["hint_action"]; nodeHintAction && nodeHintAction.IsScalar())
            {
                auto const actionStr = core::toUpper(nodeHintAction.as<std::string>());
                static auto constexpr HintActionMappings =
                    std::array<std::pair<std::string_view, vtbackend::HintAction>, 5> { {
                        { "COPY", vtbackend::HintAction::Copy },
                        { "OPEN", vtbackend::HintAction::Open },
                        { "PASTE", vtbackend::HintAction::Paste },
                        { "COPYANDPASTE", vtbackend::HintAction::CopyAndPaste },
                        { "SELECT", vtbackend::HintAction::Select },
                    } };
                if (auto const p = std::ranges::find_if(HintActionMappings,
                                                        [&](auto const& t) { return t.first == actionStr; });
                    p != HintActionMappings.end())
                {
                    hintAction = p->second;
                }
                else
                {
                    logger()("Invalid hint_action '{}' in HintMode action. Defaulting to 'Copy'.",
                             nodeHintAction.as<std::string>());
                }
            }

            auto scope = vtbackend::HintScope::Visible;
            if (auto nodeScope = node["scope"]; nodeScope && nodeScope.IsScalar())
            {
                auto const scopeStr = core::toUpper(nodeScope.as<std::string>());
                static auto constexpr HintScopeMappings =
                    std::array<std::pair<std::string_view, vtbackend::HintScope>, 2> { {
                        { "VISIBLE", vtbackend::HintScope::Visible },
                        { "SCROLLBACK", vtbackend::HintScope::Scrollback },
                    } };
                if (auto const p = std::ranges::find_if(HintScopeMappings,
                                                        [&](auto const& t) { return t.first == scopeStr; });
                    p != HintScopeMappings.end())
                {
                    scope = p->second;
                }
                else
                {
                    logger()("Invalid scope '{}' in HintMode action. Defaulting to 'Visible'.",
                             nodeScope.as<std::string>());
                }
            }

            auto patterns = std::string {};
            if (auto nodePatterns = node["patterns"]; nodePatterns && nodePatterns.IsScalar())
                patterns = nodePatterns.as<std::string>();

            return actions::HintMode { .patterns = std::move(patterns),
                                       .hintAction = hintAction,
                                       .scope = scope };
        }

        // The block actions read their arguments through one visit; any other action is complete as named.
        return withCommandBlockArguments(node, action);
    }
    return std::nullopt;
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     contour::config::SelectionAction& where)
{
    auto const child = node[entry];
    if (child)
    {

        auto const value = core::toUpper(child.as<std::string>());
        auto constexpr Mappings = std::array {
            std::pair { "COPYTOCLIPBOARD", contour::config::SelectionAction::CopyToClipboard },
            std::pair { "COPYTOSELECTIONCLIPBOARD",
                        contour::config::SelectionAction::CopyToSelectionClipboard },
            std::pair { "NOTHING", contour::config::SelectionAction::Nothing },
        };
        logger()("Loading entry: {}, value {}", entry, value);
        bool found = false;
        for (auto const& mapping: Mappings)
            if (mapping.first == value)
            {
                where = mapping.second;
                found = true;
            }
        if (!found)
            where = contour::config::SelectionAction::Nothing;
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::CursorShape& where)
{

    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::CursorShape> {
        auto const upperKey = core::toUpper(key);
        logger()("Loading entry: {}, value {}", entry, upperKey);
        if (upperKey == "BLOCK")
            return vtbackend::CursorShape::Block;
        if (upperKey == "RECTANGLE")
        {
            return vtbackend::CursorShape::Rectangle;
        }
        if (upperKey == "UNDERSCORE")
            return vtbackend::CursorShape::Underscore;
        if (upperKey == "BAR")
            return vtbackend::CursorShape::Bar;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::Modifiers& where)
{
    if (auto const child = node[entry])
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::CursorDisplay& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::CursorDisplay> {
        auto const upperKey = core::toUpper(key);
        logger()("Loading entry: {}, value {}", entry, upperKey);
        if (upperKey == "TRUE")
            return vtbackend::CursorDisplay::Blink;
        if (upperKey == "FALSE")
            return vtbackend::CursorDisplay::Steady;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::BlinkStyle& where)
{
    auto parse = [&](std::string const& key) -> std::optional<vtbackend::BlinkStyle> {
        auto const upperKey = core::toUpper(key);
        logger()("Loading entry: {}, value {}", entry, upperKey);
        if (upperKey == "CLASSIC")
            return vtbackend::BlinkStyle::Classic;
        if (upperKey == "SMOOTH")
            return vtbackend::BlinkStyle::Smooth;
        if (upperKey == "LINGER")
            return vtbackend::BlinkStyle::Linger;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto opt = parse(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::ScreenTransitionStyle& where)
{
    auto parse = [&](std::string const& key) -> std::optional<vtbackend::ScreenTransitionStyle> {
        auto const upperKey = core::toUpper(key);
        if (upperKey == "CLASSIC")
            return vtbackend::ScreenTransitionStyle::Classic;
        if (upperKey == "FADE")
            return vtbackend::ScreenTransitionStyle::Fade;
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto const value = child.as<std::string>();
        auto const opt = parse(value);
        if (opt.has_value())
            where = opt.value();
        else
            logger()("Unrecognized screen transition style: {}", value);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     crispy::LRUCapacity& where)
{
    if (auto const child = node[entry])
        where.value = child.as<uint32_t>();
    logger()("Loading entry: {}, value {}", entry, where.value);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::MaxHistoryLineCount& where)
{
    if (auto const child = node[entry])
    {
        auto value = child.as<int>();
        if (value == -1)
            where = vtbackend::Infinite {};
        else
            where = vtbackend::LineCount(value);
    }
    if (std::holds_alternative<vtbackend::Infinite>(where))
        logger()("Loading entry: {}, value {}", entry, "Infinity");
    else
        logger()("Loading entry: {}, value {}", entry, std::get<vtbackend::LineCount>(where));
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     crispy::StrongHashtableSize& where)
{
    if (auto const child = node[entry])
        where.value = child.as<uint32_t>();
    logger()("Loading entry: {}, value {}", entry, where.value);
}

template <typename Writer>
static std::string createForGlobal(Config const& c)
{
    auto doc = std::string {};
    Writer writer;

    auto const escapeSequence = [&](std::string const& value) {
        /* \ -> \\ */
        /* " -> \" */
        return std::regex_replace(
            std::regex_replace(value, std::regex("\\\\"), "\\$&"), std::regex("\""), "\\$&");
    };

    auto const processConfigEntry =
        [&]<typename T, documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto const& name,
            contour::config::ConfigEntry<
                T,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& v) {
            doc.append(writer.processEntry(writer.whichDoc(v), name, v.value()));
        };

    auto const processConfigEntryWithEscape =
        [&]<documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto const& name,
            contour::config::ConfigEntry<
                std::string,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& v) {
            doc.append(writer.processEntry(writer.whichDoc(v), name, escapeSequence(v.value())));
        };

    // Not a ConfigEntry: the edition is fixed before the file is read, so no loader assigns it. It is
    // written all the same, so that a generated file names the defaults it was generated with.
    auto const processDefaultsEdition = [&](auto const& name, DefaultsEdition const& edition) {
        doc.append(writer.processEntry(writer.whichDoc(documentation::DefaultsEdition {}), name, edition));
    };

    auto completeOverload = core::Overloaded {
        processConfigEntry,
        processConfigEntryWithEscape,
        processDefaultsEdition,
        // Ignored entries
        [&]([[maybe_unused]] auto const& name,
            [[maybe_unused]] ConfigEntry<std::unordered_map<std::string, vtbackend::ColorPalette>,
                                         documentation::ColorSchemes> const& v) {},
        [&]([[maybe_unused]] auto const& name,
            [[maybe_unused]] ConfigEntry<std::map<vtbackend::DECMode, bool>,
                                         documentation::FrozenDecMode> const& v) {},
        [&]([[maybe_unused]] auto const& name,
            [[maybe_unused]] ConfigEntry<InputMappings, documentation::InputMappings> const& v) {},
        [&]([[maybe_unused]] auto const& name,
            [[maybe_unused]] ConfigEntry<std::unordered_map<std::string, TerminalProfile>,
                                         documentation::Profiles> const& v) {},
        [&]([[maybe_unused]] auto const& name,
            [[maybe_unused]] ConfigEntry<std::unordered_map<std::string, config::Layout>,
                                         documentation::Layouts> const& v) {},
        [&]([[maybe_unused]] auto const& name,
            [[maybe_unused]] ConfigEntry<std::map<std::string, HostEntry>, documentation::Hosts> const& v) {},
        [&]([[maybe_unused]] auto const& name, [[maybe_unused]] auto const& v) {},
    };

    Reflection::CallOnMembers(c, completeOverload);

    return doc;
}

/// Appends one profile's body (every ConfigEntry member, walked via reflection) to @p doc using
/// @p writer. The single source of truth for profile serialization, shared by createForProfile
/// (which wraps it in `profiles:`/name scopes) and createForSingleProfile (which emits it bare for a
/// profiles/<name>.yml side file). Because it is a reflection walk, a newly-added profile field is
/// serialized here automatically — there is nothing to hand-list.
/// @param writer The writer providing indentation and value formatting.
/// @param doc The output buffer to append to.
/// @param profile The profile to serialize.
template <typename Writer>
static void emitProfileBody(Writer& writer, std::string& doc, TerminalProfile const& profile)
{
    auto const processConfigEntry =
        [&]<typename T, documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto const& name,
            contour::config::ConfigEntry<
                T,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& v) {
            doc.append(writer.processEntry(writer.whichDoc(v), name, v.value()));
        };

    // Something is wrong for vtpty::Process::ExecInfo formatting for static build
    // we add this lambda to handle it in overload set for now
    auto const processConfigEntryWithExecInfo =
        [&]<documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto const& name,
            contour::config::ConfigEntry<
                vtpty::Process::ExecInfo,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& vEntry) {
            auto v = vEntry.value();
            auto args = std::string { "[" };
            args.append(v.arguments | core::views::joinWith(", "sv));
            args.append("]");

            doc.append(writer.processEntry(
                writer.whichDoc(vEntry), name, v.program, args, spelledWorkingDirectory(v.workingDirectory)));
        };

    auto completeOverload = core::Overloaded {
        processConfigEntryWithExecInfo,
        processConfigEntry,
        [&]([[maybe_unused]] auto const& name, [[maybe_unused]] auto const& v) {},
    };

    Reflection::CallOnMembers(profile, completeOverload);
}

/// Serializes one profile's body as a bare top-level map for a `profiles/<name>.yml` GUI side file
/// (no `profiles:`/name wrapper), so the same reader that parses an inline profile parses it back.
template <typename Writer>
static std::string createForSingleProfile(TerminalProfile const& profile)
{
    auto doc = std::string {};
    Writer writer;
    emitProfileBody(writer, doc, profile);
    return doc;
}

template <typename Writer>
static std::string createForProfile(Config const& c)
{
    auto doc = std::string {};
    Writer writer;

    // inside profiles:
    doc.append(writer.replaceCommentPlaceholder(std::string { writer.whichDoc(c.profiles) }));
    {
        auto const _ = typename Writer::Offset {};
        for (auto&& [name, entry]: c.profiles.value())
        {
            if constexpr (std::same_as<Writer, YAMLConfigWriter>)
                doc.append(std::format("    {}: \n", name));

            writer.scoped([&]() { emitProfileBody(writer, doc, entry); });
        }
    }
    return doc;
}

/// Appends one color palette's body (every colored slot: defaults, decorations, highlights, the
/// 3×8 ANSI colors) to @p doc via @p writer. The single source of truth for palette serialization,
/// shared by createForColorScheme (which wraps it in `color_schemes:`/name scopes) and
/// createForSingleColorScheme (which emits it bare for a colorschemes/<name>.yml side file), so a new
/// palette slot is added in exactly one place.
/// @param writer The writer providing indentation and value formatting.
/// @param doc The output buffer to append to.
/// @param entry The palette to serialize.
template <typename Writer>
static void emitColorPaletteBody(Writer& writer, std::string& doc, vtbackend::ColorPalette const& entry)
{
    auto const processWithDoc = [&](auto&& docString, auto... val) {
        doc.append(writer.replaceCommentPlaceholder(writer.process(writer.whichDoc(docString), val...)));
    };

    processWithDoc(documentation::DefaultColors {},
                   entry.defaultBackground,
                   entry.defaultForeground,
                   entry.defaultForegroundBright,
                   entry.defaultForegroundDimmed);

    processWithDoc(documentation::HyperlinkDecorationColors {},
                   entry.hyperlinkDecoration.normal,
                   entry.hyperlinkDecoration.hover);

    processWithDoc(documentation::YankHighlight {},
                   entry.yankHighlight.foreground,
                   entry.yankHighlight.foregroundAlpha,
                   entry.yankHighlight.background,
                   entry.yankHighlight.backgroundAlpha);

    processWithDoc(documentation::NormalModeCursorline {},
                   entry.normalModeCursorline.foreground,
                   entry.normalModeCursorline.foregroundAlpha,
                   entry.normalModeCursorline.background,
                   entry.normalModeCursorline.backgroundAlpha);

    processWithDoc(documentation::Selection {},
                   entry.selection.foreground,
                   entry.selection.foregroundAlpha,
                   entry.selection.background,
                   entry.selection.backgroundAlpha);

    processWithDoc(documentation::SearchHighlight {},
                   entry.searchHighlight.foreground,
                   entry.searchHighlight.foregroundAlpha,
                   entry.searchHighlight.background,
                   entry.searchHighlight.backgroundAlpha);

    processWithDoc(documentation::SearchHighlightFocused {},
                   entry.searchHighlightFocused.foreground,
                   entry.searchHighlightFocused.foregroundAlpha,
                   entry.searchHighlightFocused.background,
                   entry.searchHighlightFocused.backgroundAlpha);

    processWithDoc(documentation::WordHighlightCurrent {},
                   entry.wordHighlightCurrent.foreground,
                   entry.wordHighlightCurrent.foregroundAlpha,
                   entry.wordHighlightCurrent.background,
                   entry.wordHighlightCurrent.backgroundAlpha);

    processWithDoc(documentation::WordHighlight {},
                   entry.wordHighlight.foreground,
                   entry.wordHighlight.foregroundAlpha,
                   entry.wordHighlight.background,
                   entry.wordHighlight.backgroundAlpha);

    processWithDoc(documentation::HintLabel {},
                   entry.hintLabel.foreground,
                   entry.hintLabel.foregroundAlpha,
                   entry.hintLabel.background,
                   entry.hintLabel.backgroundAlpha);

    processWithDoc(documentation::HintMatch {},
                   entry.hintMatch.foreground,
                   entry.hintMatch.foregroundAlpha,
                   entry.hintMatch.background,
                   entry.hintMatch.backgroundAlpha);

    processWithDoc(documentation::IndicatorStatusLine {},
                   entry.indicatorStatusLineInsertMode.foreground,
                   entry.indicatorStatusLineInsertMode.background,
                   entry.indicatorStatusLineInactive.foreground,
                   entry.indicatorStatusLineInactive.background);

    // Written back key by key, and only for what was actually set. The two pairs are independent
    // optionals -- a scheme that says nothing about folding derives the column from its own
    // foreground/background -- so emitting a full block would pin colours the scheme never chose, and
    // emitting the commented example unconditionally would drop the ones it did.
    if (entry.foldMarker || entry.foldMarkerHover)
    {
        processWithDoc(documentation::FoldMarkerHeader {});
        if (entry.foldMarker)
            processWithDoc(documentation::FoldMarkerDefault {},
                           entry.foldMarker->foreground,
                           entry.foldMarker->background);
        if (entry.foldMarkerHover)
            processWithDoc(documentation::FoldMarkerHover {},
                           entry.foldMarkerHover->foreground,
                           entry.foldMarkerHover->background);
    }
    else
        processWithDoc(documentation::FoldMarker {});

    // A map of optional colours, by the same rule for the same reason: under its header only the keys the
    // scheme set, and the commented example when it set none.
    auto const emitOptionalColorMap = [&](auto header, auto example, auto const& rows) {
        if (std::ranges::none_of(rows, [](auto const& row) { return row.second.has_value(); }))
        {
            processWithDoc(example);
            return;
        }
        processWithDoc(header);
        for (auto const& [key, color]: rows)
            if (color)
                processWithDoc(documentation::IndentedEntry {}, key, *color);
    };

    emitOptionalColorMap(documentation::BlockStatusHeader {},
                         documentation::BlockStatus {},
                         BlockStatusKeys | std::views::transform([&](auto const& row) {
                             return std::pair { row.first,
                                                entry.blockStatus[static_cast<size_t>(row.second)] };
                         }));
    emitOptionalColorMap(documentation::BlockStatusHoverHeader {},
                         documentation::BlockStatusHover {},
                         BlockStatusKeys | std::views::transform([&](auto const& row) {
                             return std::pair { row.first,
                                                entry.blockStatusHover[static_cast<size_t>(row.second)] };
                         }));

    if (entry.gutterText)
        processWithDoc(documentation::GutterTextSet {}, *entry.gutterText);
    else
        processWithDoc(documentation::GutterText {});

    emitOptionalColorMap(documentation::StickyHeaderColorsHeader {},
                         documentation::StickyHeaderColors {},
                         StickyHeaderColorSlots | std::views::transform([&](auto const& row) {
                             return std::pair { row.first, entry.*row.second };
                         }));

    emitOptionalColorMap(documentation::ContextTintHeader {},
                         documentation::ContextTint {},
                         vtbackend::ContextTypeList | std::views::transform([&](auto const type) {
                             return std::pair { vtbackend::contextTypeName(type),
                                                entry.contextTints[static_cast<size_t>(type)] };
                         }));

    processWithDoc(documentation::InputMethodEditor {},
                   entry.inputMethodEditor.foreground,
                   entry.inputMethodEditor.background);

    processWithDoc(documentation::NormalColors {},
                   entry.normalColor(0),
                   entry.normalColor(1),
                   entry.normalColor(2),
                   entry.normalColor(3),
                   entry.normalColor(4),
                   entry.normalColor(5),
                   entry.normalColor(6),
                   entry.normalColor(7));

    processWithDoc(documentation::BrightColors {},
                   entry.brightColor(0),
                   entry.brightColor(1),
                   entry.brightColor(2),
                   entry.brightColor(3),
                   entry.brightColor(4),
                   entry.brightColor(5),
                   entry.brightColor(6),
                   entry.brightColor(7));

    processWithDoc(documentation::DimColors {},
                   entry.dimColor(0),
                   entry.dimColor(1),
                   entry.dimColor(2),
                   entry.dimColor(3),
                   entry.dimColor(4),
                   entry.dimColor(5),
                   entry.dimColor(6),
                   entry.dimColor(7));
}

/// Serializes one color palette as a bare top-level body for a `colorschemes/<name>.yml` GUI side
/// file (no `color_schemes:`/name wrapper), matching that file's lazy read path.
template <typename Writer>
static std::string createForSingleColorScheme(vtbackend::ColorPalette const& palette)
{
    auto doc = std::string {};
    Writer writer;
    emitColorPaletteBody(writer, doc, palette);
    return doc;
}

template <typename Writer>
static std::string createForColorScheme(Config const& c)
{
    auto doc = std::string {};
    Writer writer;

    doc.append(writer.replaceCommentPlaceholder(c.colorschemes.documentation));
    writer.scoped([&]() {
        for (auto&& [name, entry]: c.colorschemes.value())
        {
            doc.append(std::format("    {}: \n", name));
            {
                auto const _ = typename Writer::Offset {};
                emitColorPaletteBody(writer, doc, entry);
            }
        }
    });

    return doc;
}

template <typename Writer>
static std::string createKeyMapping(Config const& c)
{
    auto doc = std::string {};
    Writer writer;

    doc.append(writer.replaceCommentPlaceholder(c.inputMappings.documentation));
    {
        auto const _ = typename Writer::Offset {};
        for (auto&& entry: c.inputMappings.value().keyMappings)
            doc.append(Writer::addOffset(writer.format(entry), Writer::Offset::Levels * Writer::OneOffset));

        for (auto&& entry: c.inputMappings.value().charMappings)
            doc.append(Writer::addOffset(writer.format(entry), Writer::Offset::Levels * Writer::OneOffset));

        for (auto&& entry: c.inputMappings.value().mouseMappings)
            doc.append(Writer::addOffset(writer.format(entry), Writer::Offset::Levels * Writer::OneOffset));
    }

    return doc;
}

template <typename Writer>
std::string documentationGlobalConfig(Config const& c)
{
    return createForGlobal<Writer>(c);
}

template <typename Writer>
std::string documentationProfileConfig(Config const& c)
{
    return createForProfile<Writer>(c);
}

template <typename Writer>
std::string createString(Config const& c)
{
    return createForGlobal<Writer>(c) + createForProfile<Writer>(c) + createForColorScheme<Writer>(c)
           + createKeyMapping<Writer>(c);
}

std::string emitProfileYaml(TerminalProfile const& profile)
{
    return createForSingleProfile<YAMLConfigWriter>(profile);
}

std::string emitColorSchemeYaml(vtbackend::ColorPalette const& palette)
{
    return createForSingleColorScheme<YAMLConfigWriter>(palette);
}

std::string emitGuiSettingsYaml(GuiManagedSettings const& settings)
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    if (settings.defaultProfile)
        out << YAML::Key << "default_profile" << YAML::Value << *settings.defaultProfile;
    // A dotted key ("renderer.gpu") is a nested setting: each section is written once, as a map, so
    // settings.yml reads exactly like the contour.yml it overrides.
    auto nested = std::map<std::string, std::map<std::string, std::string>> {};
    for (auto const& [key, value]: settings.globalOverrides)
        if (auto const dot = key.find('.'); dot != std::string::npos)
            nested[key.substr(0, dot)][key.substr(dot + 1)] = value;
    // A flat key that also names a section is a hand-edited scalar (`renderer: foo`) the page has since set
    // a key under. The section wins: it is what the page just set, and the scalar written beside it would
    // be the key a load finds first.
    for (auto const& [key, value]: settings.globalOverrides)
        if (!key.contains('.') && !nested.contains(key))
            out << YAML::Key << key << YAML::Value << value;
    for (auto const& [section, entries]: nested)
    {
        out << YAML::Key << section << YAML::Value << YAML::BeginMap;
        for (auto const& [name, value]: entries)
            out << YAML::Key << name << YAML::Value << value;
        out << YAML::EndMap;
    }
    out << YAML::EndMap;
    return std::string { out.c_str() } + '\n';
}

std::optional<vtbackend::ColorPalette> loadColorSchemeFile(std::filesystem::path const& path)
{
    auto const document = YAMLConfigReader::parseSideFile(path);
    if (!document)
    {
        configLog()("Could not read color scheme {}", document.error());
        return std::nullopt;
    }

    try
    {
        auto reader = YAMLConfigReader(path, *document, configLog, configEnvironment());
        auto palette = vtbackend::ColorPalette {};
        reader.loadFromEntry(*document, palette);
        return palette;
    }
    catch (std::exception const& e)
    {
        configLog()("Could not read color scheme {}: {}", core::pathToUtf8(path), e.what());
        return std::nullopt;
    }
}

std::expected<GuiManagedSettings, std::string> loadGuiSettingsFile(std::filesystem::path const& path)
{
    return parseGuiSettingsFile(path).transform(guiSettingsFrom);
}

} // namespace contour::config
