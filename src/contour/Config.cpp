// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/Config.h>
#include <contour/ModifierNames.h>

#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>

#include <vtpty/ImageSize.h>

#include <text_shaper/font.h>

#include <crispy/StrongHash.h>
#include <crispy/escape.h>

#include <yaml-cpp/emitter.h>

#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

#include <algorithm>
#include <array>
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

auto constexpr MinimumFontSize = text::font_size { 8.0 };

using namespace std;
using crispy::homeResolvedPath;
using crispy::replaceVariables;

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
    };
    return mappings;
}

namespace
{
    /// Matches @p input against the enabled rows of @p table.
    ///
    /// Shared by both applyBuiltinFallback overloads so the gating pipeline -- filter by the row's
    /// predicate, then match -- exists once rather than once per input kind.
    template <typename Input>
    [[nodiscard]] ActionList const* applyFallbackTable(std::vector<FallbackMapping<Input>> const& table,
                                                       Config const& config,
                                                       Input input,
                                                       vtbackend::Modifiers modifiers,
                                                       uint8_t actualModeFlags)
    {
        auto enabledMappings =
            table
            | std::views::filter([&config](FallbackMapping<Input> const& row) { return row.enabled(config); })
            | std::views::transform(&FallbackMapping<Input>::mapping);
        return apply(enabledMappings, input, modifiers, actualModeFlags);
    }
} // namespace

ActionList const* applyBuiltinFallback(Config const& config,
                                       vtbackend::MouseButton button,
                                       vtbackend::Modifiers modifiers,
                                       uint8_t actualModeFlags)
{
    return applyFallbackTable(builtinFallbackMouseMappings(), config, button, modifiers, actualModeFlags);
}

ActionList const* applyBuiltinFallback(Config const& config,
                                       vtbackend::Key key,
                                       vtbackend::Modifiers modifiers,
                                       uint8_t actualModeFlags)
{
    return applyFallbackTable(builtinFallbackKeyMappings(), config, key, modifiers, actualModeFlags);
}

namespace
{

    auto const configLog = logstore::category("config", "Logs configuration file loading.");

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
        auto const lowered = crispy::toLower(name);
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
               | crispy::views::join_with(", ");
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
        static constexpr auto Fields = std::array { "action"sv, "key"sv, "mouse"sv, "mods"sv, "mode"sv };

        auto described = std::vector<std::string> {};
        for (auto const field: Fields)
            if (auto const value = row[std::string { field }]; value && value.IsScalar())
                described.emplace_back(std::format("{}: {}", field, value.as<std::string>()));

        if (described.empty())
            return "<empty>";
        return described | crispy::views::join_with(", ");
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
                throw runtime_error { std::format(
                    "Could not create directory {}. {}", path.parent_path().string(), ec.message()) };
    }

    std::vector<fs::path> getTermInfoDirs(optional<fs::path> const& appTerminfoDir)
    {
        auto locations = std::vector<fs::path>();

        if (appTerminfoDir.has_value())
            locations.emplace_back(appTerminfoDir.value().string());

        locations.emplace_back(Process::homeDirectory() / ".terminfo");

        // qEnvironmentVariable() rather than getenv(): the latter is not thread safe, and this runs
        // on config (re)load while the PTY threads are live.
        if (auto const terminfoDirs = qEnvironmentVariable("TERMINFO_DIRS").toStdString();
            !terminfoDirs.empty())
            for (auto const dir: crispy::split(string_view(terminfoDirs), ':'))
                locations.emplace_back(string(dir));

        locations.emplace_back("/usr/share/terminfo");

        // BSD locations
        locations.emplace_back("/usr/local/share/terminfo");
        locations.emplace_back("/usr/local/share/site-terminfo");

        return locations;
    }

    string getDefaultTERM(optional<fs::path> const& appTerminfoDir)
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
    if (auto const value = qEnvironmentVariable("XDG_CONFIG_HOME"); !value.isEmpty())
        return fs::path { value.toStdString() } / programName;
    else
        return Process::homeDirectory() / ".config" / programName;
#endif

#ifdef _WIN32
    DWORD size = GetEnvironmentVariableA("LOCALAPPDATA", nullptr, 0);
    if (size)
    {
        std::vector<char> buf;
        buf.resize(size);
        GetEnvironmentVariableA("LOCALAPPDATA", &buf[0], size);
        return fs::path { &buf[0] } / programName;
    }
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

    ofstream { path.string(), ios::binary | ios::trunc } << defaultConfigString();

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
    return (configHome() / "contour.yml").string();
}

Config loadConfig()
{
    return loadConfigFromFile(defaultConfigFilePath());
}

Config loadConfigFromFile(fs::path const& fileName)
{
    Config config {};

    loadConfigFromFile(config, fileName);

    return config;
}

void compareEntries(Config& config, auto const& output)
{
    // compare entries in config and default config and log differences
    auto defaultConfig = YAML::Load(defaultConfigString());
    auto userConfig = YAML::LoadFile(config.configFile.string());

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
    auto base = TerminalProfile {};
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
            auto const name = path.stem().string();
            auto const contents = readFile(path);
            if (!contents)
            {
                configLog()("Skipping unreadable profile side file: {}", path.string());
                continue;
            }
            auto root = YAML::Node {};
            try
            {
                root = YAML::Load(*contents);
            }
            catch (std::exception const& e)
            {
                configLog()("Skipping malformed profile side file {}: {}", path.string(), e.what());
                continue;
            }
            auto profile = base; // inherit the default profile's values, then overlay the file's keys
            reader.loadProfileBody(root, profile);
            config.profiles.value()[name] = std::move(profile);
            config.profileOrigins[name] = SettingsOrigin::SideFile;
        }
    }

    // settings.yml — GUI-owned globals. Applied last so a default_profile can name a profile the scan
    // above just introduced.
    auto const settingsPath = dir / "settings.yml";
    if (auto loaded = loadGuiSettingsFile(settingsPath))
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
        // new overridable global is just another loadFromEntry line here.
        std::error_code ec;
        if (!loaded->globalOverrides.empty() && std::filesystem::exists(settingsPath, ec) && !ec)
        {
            auto overrides = YAMLConfigReader(settingsPath.string(), configLog);
            overrides.loadFromEntry("word_delimiters", config.wordDelimiters);
            overrides.loadFromEntry("read_buffer_size", config.ptyReadBufferSize);
            overrides.loadFromEntry("pty_buffer_size", config.ptyBufferObjectSize);
            overrides.loadFromEntry("command_palette_recent_count", config.commandPaletteRecentCount);
            overrides.loadFromEntry("spawn_new_process", config.spawnNewProcess);
            overrides.loadFromEntry("reflow_on_resize", config.reflowOnResize);
            overrides.loadFromEntry("tab_switch_on_horizontal_wheel", config.tabSwitchOnHorizontalWheel);
            overrides.loadFromEntry("accessibility_announcements", config.accessibilityAnnouncements);
            overrides.loadFromEntry("hyperlink_hover_tooltip", config.hyperlinkHoverTooltip);
            overrides.loadFromEntry("tab_bar_position", config.tabBarPosition);
            overrides.loadFromEntry("tab_bar_visibility", config.tabBarVisibility);
            overrides.loadFromEntry("theme", config.theme);
            overrides.loadFromEntry("early_exit_threshold", config.earlyExitThreshold);
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
    logger()("Loading configuration from file: {} ", fileName.string());
    config.configFile = fileName;
    createFileIfNotExists(config.configFile);

    // A live reload reuses the same Config object rather than a fresh one (see reloadAllSessions), so the
    // collections rebuilt wholesale from the file plus the GUI side-file scan below must be reset to their
    // default-constructed starting state first. Otherwise a profile or color scheme that was deleted from
    // disk (e.g. a GUI entity removed in the settings page) would linger from the previous load, and its
    // stale provenance would keep the settings page treating a contour.yml entry as GUI-owned. The loaders
    // assume the built-in seeds exist (the "main" profile is the inheritance base; the "default" color
    // palette is the fallback), so restore those — exactly a first load's initial Config state.
    config.profiles.value() = { { "main", TerminalProfile {} } };
    config.colorschemes.value() = { { "default", vtbackend::ColorPalette {} } };
    config.profileOrigins.clear();
    config.colorSchemeOrigins.clear();

    auto yamlVisitor = YAMLConfigReader(config.configFile.string(), logger);
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
    try
    {
        YAML::LoadFile(path.string());
    }
    catch (std::exception const& e)
    {
        return std::unexpected(std::string(e.what()));
    }

    auto reader = YAMLConfigReader(path.string(), configLog);
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

YAMLConfigReader::YAMLConfigReader(std::string const& filename,
                                   logstore::category const& log,
                                   VariableReplacer replacer):
    configFile(filename), logger { log }, variableReplacer { std::move(replacer) }
{
    if (!variableReplacer)
    {
        variableReplacer = [&log = logger](std::string_view name) -> std::string {
            auto const key = std::string(name);
            if (qEnvironmentVariableIsSet(key.c_str()))
                return qEnvironmentVariable(key.c_str()).toStdString();
            log()("Undefined environment variable: ${{{}}}", name);
            return {};
        };
    }
    try
    {
        // Read through readFile (binary + CRLF->LF normalization) rather than YAML::LoadFile, so every
        // config document — contour.yml, the settings.yml global-override reader, and the color-scheme
        // files — is parsed identically regardless of how it was saved. YAML::LoadFile is CRLF-safe only
        // on Windows (its text-mode ifstream strips '\r' at the OS layer); a CRLF file synced onto
        // Linux/macOS would otherwise leave a trailing '\r' on plain scalar values and fail typed
        // conversion. This is the same normalization the GUI profile side files already rely on.
        if (auto const contents = readFile(configFile))
            doc = YAML::Load(*contents);
        else
            errorLog()("Configuration file could not be read.\nDefault config will be loaded.");
    }
    catch (std::exception const& e)
    {
        errorLog()("Configuration file is corrupted. {}\nDefault config will be loaded.", e.what());
    }
}

std::string YAMLConfigReader::resolveVariables(std::string const& input) const
{
    return replaceVariables(input, variableReplacer);
}

std::filesystem::path YAMLConfigReader::resolvedPath(std::string const& input) const
{
    return homeResolvedPath(resolveVariables(input), vtpty::Process::homeDirectory());
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
        auto const renderBackendStr = crispy::toUpper(rawValue);
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
        loadFromEntry("live_config", c.live);
        loadFromEntry("early_exit_threshold", c.earlyExitThreshold);
        loadFromEntry("spawn_new_process", c.spawnNewProcess);
        loadFromEntry("reflow_on_resize", c.reflowOnResize);
        loadFromEntry("tab_switch_on_horizontal_wheel", c.tabSwitchOnHorizontalWheel);
        loadFromEntry("accessibility_announcements", c.accessibilityAnnouncements);
        loadFromEntry("hyperlink_hover_tooltip", c.hyperlinkHoverTooltip);
        loadFromEntry("tab_bar_position", c.tabBarPosition);
        loadFromEntry("tab_bar_visibility", c.tabBarVisibility);
        loadFromEntry("text_scaling_method", c.textScalingMethod);
        loadFromEntry("grapheme_clustering", c.graphemeClustering);
        loadFromEntry("gui_config_locked", c.guiConfigLocked);
        loadFromEntry("theme", c.theme);
        loadFromEntry("experimental", c.experimentalFeatures);
        loadFromEntry("bypass_mouse_protocol_modifier", c.bypassMouseProtocolModifiers);
        loadFromEntry("on_mouse_select", c.onMouseSelection);
        loadFromEntry("mouse_block_selection_modifier", c.mouseBlockSelectionModifiers);
        loadFromEntry("profiles", c.profiles, c.defaultProfileName.value());
        loadFromEntry("layouts", c.layouts);
        loadFromEntry("git_drawings", c.gitDrawings);
#ifdef CONTOUR_FRONTEND_GUI
        vtrasterizer::BoxDrawingRenderer::setGitDrawingsStyle(c.gitDrawings.value());
        loadFromEntry("box_arc_style", c.boxArcStyle);
        vtrasterizer::BoxDrawingRenderer::setArcStyle(c.boxArcStyle.value());
        loadFromEntry("braille_style", c.brailleStyle);
        vtrasterizer::BoxDrawingRenderer::setBrailleStyle(c.brailleStyle.value());
#endif

        // loadFromEntry("color_schemes", c.colorschemes); // NB: This is always loaded lazily
        loadFromEntry("input_mapping", c.inputMappings);
    }
    catch (std::exception const& e)
    {
        errorLog()("Something went wrong during config file loading, check `contour debug config` output "
                   "for more info");
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, TerminalProfile& where)
{
    logger()("loading profile {}\n", entry);
    loadProfileBody(node[entry], where);
}

void YAMLConfigReader::loadProfileBody(YAML::Node const& child, TerminalProfile& where)
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
        // inforce some default shell setup
        defaultSettings(where.shell.value());

        loadFromEntry(child, "escape_sandbox", where.escapeSandbox);
        loadFromEntry(child, "copy_last_mark_range_offset", where.copyLastMarkRangeOffset);
        loadFromEntry(child, "show_title_bar", where.showTitleBar);
        loadFromEntry(child, "dim_unfocused", where.dimUnfocused);
        loadFromEntry(child, "size_indicator_on_resize", where.sizeIndicatorOnResize);
        loadFromEntry(child, "fullscreen", where.fullscreen);
        loadFromEntry(child, "maximized", where.maximized);
        loadFromEntry(child, "search_mode_switch", where.searchModeSwitch);
        loadFromEntry(child, "insert_after_yank", where.insertAfterYank);
        loadFromEntry(child, "bell", where.bell);
        loadFromEntry(child, "wm_class", where.wmClass);
        loadFromEntry(child, "tab_label", where.tabLabel);
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
        if (child["cursor"])
        {
            loadFromEntry(child["cursor"], "shape", where.modeInsert.value().cursor.cursorShape);
            loadFromEntry(child["cursor"], "blinking", where.modeInsert.value().cursor.cursorDisplay);
            loadFromEntry(
                child["cursor"], "blinking_interval", where.modeInsert.value().cursor.cursorBlinkInterval);
        }
        if (child["normal_mode"] && child["normal_mode"]["cursor"])
        {
            loadFromEntry(
                child["normal_mode"]["cursor"], "shape", where.modeNormal.value().cursor.cursorShape);
            loadFromEntry(
                child["normal_mode"]["cursor"], "blinking", where.modeNormal.value().cursor.cursorDisplay);
            loadFromEntry(child["normal_mode"]["cursor"],
                          "blinking_interval",
                          where.modeNormal.value().cursor.cursorBlinkInterval);
        }
        if (child["visual_mode"] && child["visual_mode"]["cursor"])
        {
            loadFromEntry(
                child["visual_mode"]["cursor"], "shape", where.modeVisual.value().cursor.cursorShape);
            loadFromEntry(
                child["visual_mode"]["cursor"], "blinking", where.modeVisual.value().cursor.cursorDisplay);
            loadFromEntry(child["visual_mode"]["cursor"],
                          "blinking_interval",
                          where.modeVisual.value().cursor.cursorBlinkInterval);
            loadFromEntry(child["visual_mode"]["cursor"],
                          "blinking_interval",
                          where.modeVisual.value().cursor.cursorBlinkInterval);
        }
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
        loadFromEntry(child, "hint_patterns", where.hintPatterns);
    }
}

std::vector<std::string> shellSplit(std::string_view commandLine)
{
    auto tokens = std::vector<std::string> {};
    auto current = std::string {};
    auto inToken = false;

    // Consumes the quoted run whose opening quote is at rest.front(), appending its content to
    // `current`. Single quotes are literal; inside double quotes, \" and \\ are escapes. An
    // unterminated quote simply consumes the remainder.
    auto const consumeQuoted = [&current](std::string_view& rest, char const quote) {
        rest.remove_prefix(1); // the opening quote
        while (!rest.empty() && rest.front() != quote)
        {
            if (quote == '"' && rest.front() == '\\' && rest.size() >= 2
                && (rest[1] == '"' || rest[1] == '\\'))
                rest.remove_prefix(1); // drop the backslash, take the escaped character below
            current.push_back(rest.front());
            rest.remove_prefix(1);
        }
        if (!rest.empty())
            rest.remove_prefix(1); // the closing quote
    };

    auto const endToken = [&] {
        if (!inToken)
            return;
        tokens.push_back(std::move(current));
        current.clear();
        inToken = false;
    };

    for (auto rest = commandLine; !rest.empty();)
    {
        auto const c = rest.front();
        if (c == '\'' || c == '"')
        {
            inToken = true;
            consumeQuoted(rest, c);
        }
        else if (c == '\\' && rest.size() >= 2)
        {
            // Outside quotes, a backslash escapes the next character (e.g. `a\ b` is one token).
            inToken = true;
            current.push_back(rest[1]);
            rest.remove_prefix(2);
        }
        else if (c == ' ' || c == '\t')
        {
            endToken();
            rest.remove_prefix(1);
        }
        else
        {
            inToken = true;
            current.push_back(c);
            rest.remove_prefix(1);
        }
    }
    endToken();
    return tokens;
}

std::string shellQuote(std::string_view token)
{
    auto const needsQuoting = token.empty() || std::ranges::any_of(token, [](char c) {
                                  return c == ' ' || c == '\t' || c == '\'' || c == '"' || c == '\\';
                              });
    if (!needsQuoting)
        return std::string { token };
    std::string out = "'";
    for (char const c: token)
    {
        if (c == '\'')
            out += "'\\''"; // close quote, escaped literal ', reopen quote
        else
            out.push_back(c);
    }
    out += '\'';
    return out;
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
            auto const value = crispy::toLower(orientation.as<std::string>());
            if (value == "horizontal")
                where.orientation = vtmux::SplitState::Horizontal;
            else if (value == "vertical")
                where.orientation = vtmux::SplitState::Vertical;
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
        // A command may be written as a full command line ("emacs -nw"): shell-split it into the program
        // (the first token) and its arguments. Any explicit `arguments:` entries are appended after these.
        auto tokens = shellSplit(resolveVariables(command.as<std::string>()));
        if (!tokens.empty())
        {
            where.command = std::move(tokens.front());
            for (auto& token: tokens | std::views::drop(1))
                where.arguments.push_back(std::move(token));
        }
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
        auto const filePath = configFile.remove_filename() / "colorschemes" / (entry + ".yml");
        auto fileContents = readFile(filePath);
        if (!fileContents)
        {
            logger()("color palette loading failed {} ", entry);
            return;
        }
        logger()("color palette loading from file {}", filePath.string());
        try
        {
            loadFromEntry(YAML::Load(fileContents.value()), where);
            return;
        }
        catch (std::exception const& e)
        {
            logger()("Something went wrong: {}", e.what());
        }
    }

    loadFromEntry(child, where);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, vtbackend::ColorPalette& where)
{
    auto child = node;
    if (child["default"])
    {
        logger()("*** loading default colors");
        loadFromEntry(child["default"], "background", where.defaultBackground);
        loadFromEntry(child["default"], "foreground", where.defaultForeground);
        loadFromEntry(child["default"], "bright_foreground", where.defaultForegroundBright);
        loadFromEntry(child["default"], "dimmed_foreground", where.defaultForegroundDimmed);
    }

    if (child["background_image"] && child["background_image"]["path"]) // ensure that path exist
    {
        logger()("*** loading background_image");
        where.backgroundImage = std::make_shared<vtbackend::BackgroundImage>();
        loadFromEntry(child, "background_image", where.backgroundImage);
    }

    if (child["hyperlink_decoration"])
    {
        logger()("*** loading hyperlink_decoration");
        loadFromEntry(child["hyperlink_decoration"], "normal", where.hyperlinkDecoration.normal);
        loadFromEntry(child["hyperlink_decoration"], "hover", where.hyperlinkDecoration.hover);
    }

    auto loadWithLog = [&](auto& entry, auto& where) {
        logger()("*** loading {}", entry);
        loadFromEntry(child, entry, where);
    };

    loadWithLog("cursor", where.cursor);
    loadWithLog("vi_mode_highlight", where.yankHighlight);
    loadWithLog("vi_mode_cursorline", where.normalModeCursorline);
    loadWithLog("selection", where.selection);
    loadWithLog("search_highlight", where.searchHighlight);
    loadWithLog("search_highlight_focused", where.searchHighlightFocused);
    loadWithLog("word_highlight_current", where.wordHighlightCurrent);
    loadWithLog("word_highlight_other", where.wordHighlight);
    loadWithLog("hint_label", where.hintLabel);
    loadWithLog("hint_match", where.hintMatch);
    loadWithLog("input_method_editor", where.inputMethodEditor);

    if (child["indicator_statusline"])
    {
        logger()("*** loading indicator_statusline");
        if (child["indicator_statusline"]["default"])
        {
            logger()("*** loading default indicator_statusline");
            vtbackend::RGBColorPair defaultIndicatorStatusLine;
            loadFromEntry(child["indicator_statusline"], "default", defaultIndicatorStatusLine);
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
            if (child["indicator_statusline"][name])
            {
                logger()("*** loading {} indicator_statusline", name);
                loadFromEntry(child["indicator_statusline"], name, *defaultColor);
            }
        }
    }
    logger()("*** loading pallete");
    loadFromEntry(child, "", where.palette);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     [[maybe_unused]] std::string const& entry,
                                     vtbackend::ColorPalette::Palette& colors)
{
    auto const loadColorMap = [&](YAML::Node const& parent, std::string const& key, size_t offset) -> bool {
        auto node = parent[key];
        if (!node)
            return false;

        if (node.IsMap())
        {
            auto const assignColor = [&](size_t index, std::string const& name) {
                if (auto nodeValue = node[name]; nodeValue)
                {
                    if (auto const value = nodeValue.as<std::string>(); !value.empty())
                    {
                        if (value[0] == '#')
                            colors[offset + index] = value;
                        else if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
                            colors[offset + index] = vtbackend::RGBColor { nodeValue.as<uint32_t>() };
                    }
                }
            };
            assignColor(0, "black");
            assignColor(1, "red");
            assignColor(2, "green");
            assignColor(3, "yellow");
            assignColor(4, "blue");
            assignColor(5, "magenta");
            assignColor(6, "cyan");
            assignColor(7, "white");
            return true;
        }
        else if (node.IsSequence())
        {
            for (size_t i = 0; i < node.size() && i < 8; ++i)
                if (node[i].IsScalar())
                    colors[i] = vtbackend::RGBColor { node[i].as<uint32_t>() };
                else
                    colors[i] = vtbackend::RGBColor { node[i].as<std::string>() };
            return true;
        }
        return false;
    };

    logger()("*** loading normal color map");
    loadColorMap(node, "normal", 0);
    logger()("*** loading bright color map");
    loadColorMap(node, "bright", 8);
    logger()("*** loading dim color map");
    if (!loadColorMap(node, "dim", 256))
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
        auto const literal = crispy::toUpper(key);
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
        where->hash = crispy::strong_hash::compute(resolved.string());
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, Permission& where)
{
    auto parseModifierKey = [](std::string const& key) -> std::optional<Permission> {
        auto const literal = crispy::toLower(key);
        if (literal == "allow")
            return Permission::Allow;
        if (literal == "deny")
            return Permission::Deny;
        if (literal == "ask")
            return Permission::Ask;
        return std::nullopt;
    };

    if (auto const child = node[entry]; child)
    {
        if (auto const opt = parseModifierKey(child.as<std::string>()); opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::StatusDisplayType& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::StatusDisplayType> {
        auto const literal = crispy::toLower(key);
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

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtrasterizer::Decorator& where)
{
    auto parseModifierKey = [](std::string const& key) -> std::optional<vtrasterizer::Decorator> {
        auto const literal = crispy::toLower(key);

        using std::pair;
        auto constexpr Mappings = std::array {
            pair { "underline", vtrasterizer::Decorator::Underline },
            pair { "dotted-underline", vtrasterizer::Decorator::DottedUnderline },
            pair { "double-underline", vtrasterizer::Decorator::DoubleUnderline },
            pair { "curly-underline", vtrasterizer::Decorator::CurlyUnderline },
            pair { "dashed-underline", vtrasterizer::Decorator::DashedUnderline },
            pair { "overline", vtrasterizer::Decorator::Overline },
            pair { "crossed-out", vtrasterizer::Decorator::CrossedOut },
            pair { "framed", vtrasterizer::Decorator::Framed },
            pair { "encircle", vtrasterizer::Decorator::Encircle },
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == literal)
                return { mapping.second };
        return std::nullopt;
    };

    if (auto const child = node[entry])
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
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
        where.workingDirectory =
            homeResolvedPath(where.workingDirectory.generic_string(), Process::homeDirectory());
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::vector<text::font_feature>& where)
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
        auto const literal = crispy::toLower(key);

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

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, text::font_size& where)
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

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     text::font_slant& where)
{
    if (auto const child = node[entry])
    {
        auto opt = text::make_font_slant(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     text::font_weight& where)
{
    if (auto const child = node[entry])
    {
        auto opt = text::make_font_weight(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}
// NOLINTEND(readability-convert-member-functions-to-static)

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     text::font_description& where)
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
                    where.fontFallback = text::font_fallback_none {};
                }
                else if (child["fallback"].IsSequence())
                {
                    where.fontFallback = text::font_fallback_list {};
                    auto& list = std::get<text::font_fallback_list>(where.fontFallback);
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
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "tile_direct_mapping", where.textureAtlasDirectMapping);
        loadFromEntry(child, "tile_hashtable_slots", where.textureAtlasHashtableSlots);
        loadFromEntry(child, "tile_cache_count", where.textureAtlasTileCount);
        loadFromEntry(child, "backend", where.renderingBackend);
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

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, HistoryConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "limit", where.maxHistoryLineCount);
        loadFromEntry(child, "scroll_multiplier", where.historyScrollMultiplier);
        loadFromEntry(child, "auto_scroll_on_update", where.autoScrollOnUpdate);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "position", where.position);
        loadFromEntry(child, "hide_in_alt_screen", where.hideScrollbarInAltScreen);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::vector<HintPatternConfig>& where)
{
    auto const child = node[entry];
    if (child && child.IsSequence())
    {
        where.clear();
        for (auto const& item: child)
        {
            auto patternConfig = HintPatternConfig {};
            if (auto const nameNode = item["name"])
                patternConfig.name = nameNode.as<std::string>();
            if (auto const regexNode = item["regex"])
                patternConfig.regex = regexNode.as<std::string>();
            if (!patternConfig.name.empty() && !patternConfig.regex.empty())
                where.push_back(std::move(patternConfig));
            else
                logger()("Skipping hint pattern with missing name or regex: name='{}', regex='{}'",
                         patternConfig.name.empty() ? "<unnamed>" : patternConfig.name,
                         patternConfig.regex);
        }
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
        loadFromEntry(child, "normal", where.normal);
        loadFromEntry(child, "hover", where.hover);
    }
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
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtrasterizer::TextShapingEngine> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "dwrite" || literal == "directwrite")
            return vtrasterizer::TextShapingEngine::DWrite;
        if (literal == "core" || literal == "coretext")
            return vtrasterizer::TextShapingEngine::CoreText;
        if (literal == "open" || literal == "openshaper")
            return vtrasterizer::TextShapingEngine::OpenShaper;
        if (literal == "native")
            return NativeTextShapingEngine;
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
                                     vtrasterizer::FontLocatorEngine& where)
{
    auto constexpr NativeFontLocator = vtrasterizer::FontLocatorEngine::Native;
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtrasterizer::FontLocatorEngine> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        for (auto const& deprecated: { "fontconfig", "coretext", "dwrite", "directwrite" })
        {
            if (literal == deprecated)
            {
                errorLog()(R"(Setting font locator to "{}" is deprecated. Use "native".)", literal);
                return NativeFontLocator;
            }
        }
        if (literal == "native")
            return NativeFontLocator;
        if (literal == "mock")
            return vtrasterizer::FontLocatorEngine::Mock;
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
                                     text::render_mode& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<text::render_mode> {
        auto const literal = crispy::toLower(key);

        auto constexpr static Mappings = std::array {
            std::pair { "lcd", text::render_mode::lcd },
            std::pair { "light", text::render_mode::light },
            std::pair { "gray", text::render_mode::gray },
            std::pair { "", text::render_mode::gray },
            std::pair { "monochrome", text::render_mode::bitmap },
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == literal)
            {
                logger()("Loading entry: {}, value {}", entry, literal);
                return mapping.second;
            }
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
        where.bold.weight = text::font_weight::bold;
        where.italic = where.regular;
        where.italic.slant = text::font_slant::italic;
        where.boldItalic = where.regular;
        where.boldItalic.slant = text::font_slant::italic;
        where.boldItalic.weight = text::font_weight::bold;

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
        auto const literal = crispy::toLower(key);
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
        auto const literal = crispy::toLower(key);
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
        if (auto const method = vtrasterizer::methodFromName(crispy::toLower(rawValue)))
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
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "system")
            return GuiTheme::System;
        if (literal == "dark")
            return GuiTheme::Dark;
        if (literal == "light")
            return GuiTheme::Light;
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
    /// Reads a tab bar mode spelled as one of the tokens its table carries.
    ///
    /// Case-insensitive, and an unrecognized value is reported rather than silently accepted -- a typo
    /// in a visible appearance setting should not pass unnoticed. It still leaves @p where at its
    /// default, so a bad value costs a log line rather than a failed startup. Mirrors the GuiTheme
    /// reader above.
    ///
    /// @param node   The mapping to read from.
    /// @param entry  The key within @p node.
    /// @param where  Receives the mode, and is left untouched when the value is not recognized.
    /// @param logger The reader's trace category, passed in because it is a member of the reader.
    template <typename Mode>
    void loadTabBarMode(YAML::Node const& node,
                        std::string const& entry,
                        Mode& where,
                        logstore::category const& logger)
    {
        auto const child = node[entry];
        if (!child)
            return;

        auto const rawValue = child.as<std::string>();
        logger()("Loading entry: {}, value {}", entry, rawValue);
        if (auto const mode = tabBarModeFromToken<Mode>(rawValue))
            where = *mode;
        else
            errorLog()("Unknown value for {}: '{}'; keeping {}.", entry, rawValue, where);
    }
} // namespace

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, TabBarPosition& where)
{
    loadTabBarMode(node, entry, where, logger);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     TabBarVisibility& where)
{
    loadTabBarMode(node, entry, where, logger);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::StatusDisplayPosition& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::StatusDisplayPosition> {
        auto const literal = crispy::toLower(key);
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
                if (action && mods && mode)
                {
                    if (tryAddKey(where, *mode, *mods, mapping["key"], *action))
                    {
                        logger()("Adding input mapping: mods: {:<20} modifiers: {:<20} key: {:<20} "
                                 "action: {:<20}",
                                 *mods,
                                 *mode,
                                 mapping["key"].as<std::string>(),
                                 *action);
                    }
                    else if (tryAddMouse(where.mouseMappings, *mode, *mods, mapping["mouse"], *action))
                    {
                        logger()("Adding input mapping: mods: {:<20} modifiers: {:<20} mouse: {:<18} action: "
                                 "{:<20}",
                                 *mods,
                                 *mode,
                                 mapping["mouse"].as<std::string>(),
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
                    errorLog()("Dropping input_mapping entry [{}]: could not parse its {}.",
                               describeInputMappingRow(mapping),
                               unparsed | crispy::views::join_with(", "));
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
        auto const styleName = crispy::toLower(child.as<std::string>());
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
        auto const styleName = crispy::toLower(style);
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
        auto const styleName = crispy::toLower(style);
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
        auto const styleName = crispy::toLower(child.as<std::string>());
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
                                   actions::Action action)
{
    auto mouseButton = parseMouseButton(node);
    if (!mouseButton)
        return false;

    appendOrCreateBinding(bindings, modes, modifier, *mouseButton, std::move(action));
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
    auto const upperName = crispy::toUpper(node.as<std::string>());
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

    auto const text = QString::fromUtf8(name.c_str()).toUcs4();
    if (text.size() == 1)
        // Folded, because the case a letter arrives in is decided by the input route rather than by
        // the user; the lookup in TerminalSession::sendCharEvent folds the delivered codepoint to
        // match. Both sides must agree -- folding only here would break `key: 'a'` bindings that
        // work today. @see config::foldedBindingCodepoint
        return foldedBindingCodepoint(static_cast<char32_t>(text[0]));

    auto constexpr NamedChars =
        std::array { std::pair { "LESS"sv, '<' },          std::pair { "GREATER"sv, '>' },
                     std::pair { "PLUS"sv, '+' },          std::pair { "APOSTROPHE"sv, '\'' },
                     std::pair { "ADD"sv, '+' },           std::pair { "BACKSLASH"sv, 'x' },
                     std::pair { "COMMA"sv, ',' },         std::pair { "DECIMAL"sv, '.' },
                     std::pair { "DIVIDE"sv, '/' },        std::pair { "EQUAL"sv, '=' },
                     std::pair { "LEFT_BRACKET"sv, '[' },  std::pair { "MINUS"sv, '-' },
                     std::pair { "MULTIPLY"sv, '*' },      std::pair { "PERIOD"sv, '.' },
                     std::pair { "RIGHT_BRACKET"sv, ']' }, std::pair { "SEMICOLON"sv, ';' },
                     std::pair { "SLASH"sv, '/' },         std::pair { "SUBTRACT"sv, '-' },
                     std::pair { "SPACE"sv, ' ' } };

    auto const upperName = crispy::toUpper(name);
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

    auto const lowerName = crispy::toLower(name);

    for (auto const& mapping: Mappings)
        if (lowerName == crispy::toLower(mapping.first))
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
    auto const args = crispy::split(modeStr, '|');
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
        std::string const upperArg = crispy::toUpper(arg);
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
                if (auto const parsed = tabBarModeFromToken<TabBarVisibility>(mode.as<std::string>()))
                    return actions::SetTabBarVisibility { *parsed };
            return std::nullopt;
        }

        if (holds_alternative<actions::SetTabBarPosition>(action))
        {
            if (auto position = node["position"]; position && position.IsScalar())
                if (auto const parsed = tabBarModeFromToken<TabBarPosition>(position.as<std::string>()))
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
                return actions::SendChars { crispy::unescape(chars.as<std::string>()) };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::CopySelection>(action))
        {
            if (auto nodeFormat = node["format"]; nodeFormat && nodeFormat.IsScalar())
            {
                auto const formatString = crispy::toUpper(nodeFormat.as<std::string>());
                static auto constexpr Mappings =
                    std::array<std::pair<std::string_view, actions::CopyFormat>, 4> { {
                        { "TEXT", actions::CopyFormat::Text },
                        { "HTML", actions::CopyFormat::HTML },
                        { "PNG", actions::CopyFormat::PNG },
                        { "VT", actions::CopyFormat::VT },
                    } };
                // NOLINTNEXTLINE(readability-qualified-auto)
                if (auto const p =
                        std::ranges::find_if(Mappings,

                                             [&](auto const& t) { return t.first == formatString; });
                    p != Mappings.end())
                {
                    return actions::CopySelection { p->second };
                }
                logger()("Invalid format '{}' in CopySelection action. Defaulting to 'text'.",
                         nodeFormat.as<std::string>());
                return actions::CopySelection { actions::CopyFormat::Text };
            }
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
                auto const actionStr = crispy::toUpper(nodeHintAction.as<std::string>());
                static auto constexpr HintActionMappings =
                    std::array<std::pair<std::string_view, vtbackend::HintAction>, 5> { {
                        { "COPY", vtbackend::HintAction::Copy },
                        { "OPEN", vtbackend::HintAction::Open },
                        { "PASTE", vtbackend::HintAction::Paste },
                        { "COPYANDPASTE", vtbackend::HintAction::CopyAndPaste },
                        { "SELECT", vtbackend::HintAction::Select },
                    } };
                if (auto const* const p = std::ranges::find_if(
                        HintActionMappings, [&](auto const& t) { return t.first == actionStr; });
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

            auto patterns = std::string {};
            if (auto nodePatterns = node["patterns"]; nodePatterns && nodePatterns.IsScalar())
                patterns = nodePatterns.as<std::string>();

            return actions::HintMode { .patterns = std::move(patterns), .hintAction = hintAction };
        }

        return action;
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

        auto const value = crispy::toUpper(child.as<std::string>());
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
        auto const upperKey = crispy::toUpper(key);
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
        auto const upperKey = crispy::toUpper(key);
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
        auto const upperKey = crispy::toUpper(key);
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
        auto const upperKey = crispy::toUpper(key);
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
                                     crispy::lru_capacity& where)
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
                                     crispy::strong_hashtable_size& where)
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
            doc.append(writer.process(writer.whichDoc(v), name, v.value()));
        };

    auto const processConfigEntryWithEscape =
        [&]<documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto const& name,
            contour::config::ConfigEntry<
                std::string,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& v) {
            doc.append(writer.process(writer.whichDoc(v), name, escapeSequence(v.value())));
        };

    auto completeOverload = crispy::overloaded {
        processConfigEntry,
        processConfigEntryWithEscape,
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
            doc.append(writer.process(writer.whichDoc(v), name, v.value()));
        };

    // Something is wrong for vtpty::Process::ExecInfo formating for static build
    // we add this lambda to handle it in overload set for now
    auto const processConfigEntryWithExecInfo =
        [&]<documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto const& name,
            contour::config::ConfigEntry<
                vtpty::Process::ExecInfo,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& vEntry) {
            auto v = vEntry.value();
            auto args = std::string { "[" };
            args.append(v.arguments | crispy::views::join_with(", "sv));
            args.append("]");

            doc.append(writer.process(writer.whichDoc(vEntry), name, v.program, args, [&]() -> std::string {
                auto fromConfig = v.workingDirectory.string();
                if (fromConfig.empty()
                    || fromConfig == crispy::homeResolvedPath("~", vtpty::Process::homeDirectory()))
                    return std::string { "\"~\"" };
                return fromConfig;
            }()));
        };

    auto completeOverload = crispy::overloaded {
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

    processWithDoc(documentation::HyperlinkDecoration {},
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
    for (auto const& [key, value]: settings.globalOverrides)
        out << YAML::Key << key << YAML::Value << value;
    out << YAML::EndMap;
    return std::string { out.c_str() } + '\n';
}

std::optional<vtbackend::ColorPalette> loadColorSchemeFile(std::filesystem::path const& path)
{
    auto const contents = readFile(path);
    if (!contents)
        return std::nullopt;

    try
    {
        auto reader = YAMLConfigReader(path.string(), configLog);
        auto palette = vtbackend::ColorPalette {};
        reader.loadFromEntry(YAML::Load(*contents), palette);
        return palette;
    }
    catch (std::exception const& e)
    {
        configLog()("Could not read color scheme {}: {}", path.string(), e.what());
        return std::nullopt;
    }
}

std::expected<GuiManagedSettings, std::string> loadGuiSettingsFile(std::filesystem::path const& path)
{
    auto settings = GuiManagedSettings {};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return settings; // nothing saved yet is not an error

    // Read through readFile (binary + CRLF->LF normalization) so a settings.yml saved with Windows line
    // endings parses identically on every platform — matching the profile side files, and closing the
    // cross-platform CRLF gap YAML::LoadFile leaves open off Windows.
    auto const contents = readFile(path);
    if (!contents)
        return std::unexpected(std::string("Could not read settings file: ") + path.string());

    auto doc = YAML::Node {};
    try
    {
        doc = YAML::Load(*contents);
    }
    catch (std::exception const& e)
    {
        return std::unexpected(std::string(e.what()));
    }

    if (auto const node = doc["default_profile"]; node && node.IsScalar())
        settings.defaultProfile = node.as<std::string>();

    // Every other top-level scalar is a GUI global override, kept as its YAML scalar text so it can be
    // re-emitted verbatim and re-applied through the typed per-key loader on the next load.
    if (doc.IsMap())
        for (auto const& entry: doc)
        {
            auto const key = entry.first.as<std::string>();
            if (key != "default_profile" && entry.second.IsScalar())
                settings.globalOverrides[key] = entry.second.as<std::string>();
        }

    return settings;
}

} // namespace contour::config
