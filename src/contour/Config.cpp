// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/Config.h>

#include <vtbackend/ColorPalette.h>

#include <vtpty/ImageSize.h>

#include <text_shaper/font.h>

#include <crispy/StrongHash.h>
#include <crispy/escape.h>

#include <yaml-cpp/emitter.h>

#include <QtCore/QFile>
#include <QtGui/QOpenGLContext>

#include <algorithm>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
    #include <Windows.h>
#elif defined(__APPLE__)
    #include <unistd.h>

    #include <mach-o/dyld.h>
#else
    #include <unistd.h>
#endif

auto constexpr MinimumFontSize = text::font_size { 8.0 };

using namespace std;
using crispy::escape;
using crispy::homeResolvedPath;
using crispy::replaceVariables;
using crispy::toLower;
using crispy::toUpper;
using crispy::unescape;

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

    auto const configLog = logstore::category("config", "Logs configuration file loading.");

    optional<std::string> readFile(fs::path const& path)
    {
        if (!fs::exists(path))
            return nullopt;

        auto ifs = ifstream(path.string());
        if (!ifs.good())
            return nullopt;

        auto const size = fs::file_size(path);
        auto text = string {};
        text.resize(size);
        ifs.read(text.data(), static_cast<std::streamsize>(size));
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

        if (auto const* value = getenv("TERMINFO_DIRS"); value && *value)
            for (auto const dir: crispy::split(string_view(value), ':'))
                locations.emplace_back(string(dir));

        locations.emplace_back("/usr/share/terminfo");

        // BSD locations
        locations.emplace_back("/usr/local/share/terminfo");
        locations.emplace_back("/usr/local/share/site-terminfo");

        return locations;
    }

    string getDefaultTERM(optional<fs::path> const& appTerminfoDir)
    {
#if defined(_WIN32)
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

    #if defined(__APPLE__)
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
    if (auto const* value = getenv("XDG_CONFIG_HOME"); value && *value)
        return fs::path { value } / programName;
    else
        return Process::homeDirectory() / ".config" / programName;
#endif

#if defined(_WIN32)
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

std::string createString(Config const& c)
{
    return createString<YAMLConfigWriter>(c);
}

std::string defaultConfigString()
{
    const Config config {};
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

/**
 * @return success or failure of loading the config file.
 */
void loadConfigFromFile(Config& config, fs::path const& fileName)
{
    auto logger = configLog;
    logger()("Loading configuration from file: {} ", fileName.string());
    config.configFile = fileName;
    createFileIfNotExists(config.configFile);

    auto yamlVisitor = YAMLConfigReader(config.configFile.string(), logger);
    yamlVisitor.load(config);
    compareEntries(config, logger);
}

optional<std::string> readConfigFile(std::string const& filename)
{
    for (fs::path const& prefix: configHomes("contour"))
        if (auto text = readFile(prefix / filename); text.has_value())
            return text;

    return nullopt;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     std::filesystem::path& where)
{
    auto const child = node[entry];
    if (child)
    {
        where = crispy::homeResolvedPath(std::filesystem::path(child.as<std::string>()).string(),
                                         vtpty::Process::homeDirectory());
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     RenderingBackend& where)
{
    auto const child = node[entry];
    if (child)
    {
        auto renderBackendStr = crispy::toUpper(child.as<std::string>());
        if (renderBackendStr == "OPENGL")
            where = RenderingBackend::OpenGL;
        else if (renderBackendStr == "SOFTWARE")
            where = RenderingBackend::Software;

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
        loadFromEntry("renderer", c.renderer);
        loadFromEntry("word_delimiters", c.wordDelimiters);
        loadFromEntry("extended_word_delimiters", c.extendedWordDelimiters);
        loadFromEntry("read_buffer_size", c.ptyReadBufferSize);
        loadFromEntry("pty_buffer_size", c.ptyBufferObjectSize);
        loadFromEntry("images", c.images);
        loadFromEntry("live_config", c.live);
        loadFromEntry("early_exit_threshold", c.earlyExitThreshold);
        loadFromEntry("spawn_new_process", c.spawnNewProcess);
        loadFromEntry("reflow_on_resize", c.reflowOnResize);
        loadFromEntry("experimental", c.experimentalFeatures);
        loadFromEntry("bypass_mouse_protocol_modifier", c.bypassMouseProtocolModifiers);
        loadFromEntry("on_mouse_select", c.onMouseSelection);
        loadFromEntry("mouse_block_selection_modifier", c.mouseBlockSelectionModifiers);
        loadFromEntry("profiles", c.profiles, c.defaultProfileName.value());
        loadFromEntry("git_drawings", c.gitDrawings);
#if defined(CONTOUR_FRONTEND_GUI)
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
    auto const child = node[entry];
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
        loadFromEntry(child, "size_indicator_on_resize", where.sizeIndicatorOnResize);
        loadFromEntry(child, "fullscreen", where.fullscreen);
        loadFromEntry(child, "maximized", where.maximized);
        loadFromEntry(child, "search_mode_switch", where.searchModeSwitch);
        loadFromEntry(child, "insert_after_yank", where.insertAfterYank);
        loadFromEntry(child, "bell", where.bell);
        loadFromEntry(child, "wm_class", where.wmClass);
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
    loadWithLog("vi_mode_cursosrline", where.normalModeCursorline);
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
        auto resolvedPath = crispy::homeResolvedPath(filename, vtpty::Process::homeDirectory());
        where->location = resolvedPath;
        where->hash = crispy::strong_hash::compute(resolvedPath.string());
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
                                     vtpty::Process::ExecInfo& where)
{
    if (auto const child = node[entry])
    {
        where.program = child.as<std::string>();
    }
    // loading arguments from the profile
    if (auto args = node["arguments"]; args && args.IsSequence())
    {
        for (auto const& argNode: args)
            where.arguments.emplace_back(argNode.as<string>());
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
        auto const literal = crispy::toLower(key);

        using Type = vtbackend::VTType;
        auto constexpr static Mappings = std::array<std::pair<std::string_view, Type>, 10> {
            std::pair { "VT100", Type::VT100 }, std::pair { "VT220", Type::VT220 },
            std::pair { "VT240", Type::VT240 }, std::pair { "VT330", Type::VT330 },
            std::pair { "VT340", Type::VT340 }, std::pair { "VT320", Type::VT320 },
            std::pair { "VT420", Type::VT420 }, std::pair { "VT510", Type::VT510 },
            std::pair { "VT520", Type::VT520 }, std::pair { "VT525", Type::VT525 }
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
        uint width = 0;
        loadFromEntry(child, "max_width", width);
        uint height = 0;
        loadFromEntry(child, "max_height", height);
        where.maxImageSize = { .width = vtpty::Width { width }, .height = vtpty::Height { height } };
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
#if defined(_WIN32)
        vtrasterizer::TextShapingEngine::DWrite;
#elif defined(__APPLE__)
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
        loadFromEntry(child, "text_shaping.engine", where.textShapingEngine);
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

        // Clear default mappings if we are loading it
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
                        logger()("Could not add some input mapping.");
                    }
                }
            }
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
#if defined(__APPLE__)
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
    if (shell.env.find("TERM") == shell.env.end())
    {
        shell.env["TERM"] = getDefaultTERM(appTerminfoDir);
        logger()("Defaulting TERM to {}.", shell.env["TERM"]);
    }

    if (shell.env.find("COLORTERM") == shell.env.end())
        shell.env["COLORTERM"] = "truecolor";
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
        return static_cast<char32_t>(text[0]);

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
    for (const auto& i: node)
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
    using vtbackend::Modifier;
    auto const upperKey = crispy::toUpper(key);
    if (upperKey == "ALT")
        return Modifier::Alt;
    if (upperKey == "CONTROL")
        return Modifier::Control;
    if (upperKey == "SHIFT")
        return Modifier::Shift;
    if (upperKey == "SUPER")
        return Modifier::Super;
    if (upperKey == "META")
        // TODO: This is technically not correct, but we used the term Meta up until now,
        // to refer to the Windows/Cmd key. But Qt also exposes another modifier called
        // Meta, which rarely exists on modern keyboards (?), but it we need to support it
        // as well, especially since extended CSIu protocol exposes it as well.
        return Modifier::Super; // Return Modifier::Meta in the future.
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
            logger()("Unknown action '{}'.", actionNode["action"].as<std::string>());
            return std::nullopt;
        }
        auto action = actionOpt.value();
        if (holds_alternative<actions::ChangeProfile>(action))
        {
            if (auto name = node["name"]; name.IsScalar())
            {
                return actions::ChangeProfile { actionNode.as<std::string>() };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::MoveTabTo>(action))
        {
            if (auto position = node["position"]; position.IsScalar())
                return actions::MoveTabTo { position.as<int>() };
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::SwitchToTab>(action))
        {
            if (auto position = node["position"]; position.IsScalar())
            {
                return actions::SwitchToTab { position.as<int>() };
            }
            else
                return std::nullopt;
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
            if (auto profileName = node["profile"]; profileName.IsScalar())
            {
                return actions::ReloadConfig { profileName.as<std::string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::SendChars>(action))
        {
            if (auto chars = node["chars"]; chars.IsScalar())
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
                if (auto const p = std::find_if(Mappings.begin(),
                                                Mappings.end(),
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

        if (holds_alternative<actions::PasteSelection>(action))
        {
            if (auto eval = node["evaluate_in_shell"]; eval && eval.IsScalar())
            {
                return actions::PasteSelection { eval.as<bool>() };
            }
        }

        if (holds_alternative<actions::WriteScreen>(action))
        {
            if (auto chars = node["chars"]; chars.IsScalar())
            {
                return actions::WriteScreen { chars.as<std::string>() };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::CreateSelection>(action))
        {
            if (auto delimiters = node["delimiters"]; delimiters.IsScalar())
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
std::string createForGlobal(Config const& c)
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
            auto name,
            contour::config::ConfigEntry<
                T,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& v) {
            doc.append(writer.process(writer.whichDoc(v), name, v.value()));
        };

    auto const processConfigEntryWithEscape =
        [&]<documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto name,
            contour::config::ConfigEntry<
                std::string,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& v) {
            doc.append(writer.process(writer.whichDoc(v), name, escapeSequence(v.value())));
        };

    auto completeOverload = crispy::overloaded {
        processConfigEntry,
        processConfigEntryWithEscape,
        // Ignored entries
        [&]([[maybe_unused]] auto name,
            [[maybe_unused]] ConfigEntry<std::unordered_map<std::string, vtbackend::ColorPalette>,
                                         documentation::ColorSchemes> const& v) {},
        [&]([[maybe_unused]] auto name,
            [[maybe_unused]] ConfigEntry<std::map<vtbackend::DECMode, bool>,
                                         documentation::FrozenDecMode> const& v) {},
        [&]([[maybe_unused]] auto name,
            [[maybe_unused]] ConfigEntry<InputMappings, documentation::InputMappings> const& v) {},
        [&]([[maybe_unused]] auto name,
            [[maybe_unused]] ConfigEntry<std::unordered_map<std::string, TerminalProfile>,
                                         documentation::Profiles> const& v) {},
        [&]([[maybe_unused]] auto name, [[maybe_unused]] auto const& v) {},
    };

    Reflection::CallOnMembers(c, completeOverload);

    return doc;
}

template <typename Writer>
std::string createForProfile(Config const& c)
{
    auto doc = std::string {};
    Writer writer;

    auto const processConfigEntry =
        [&]<typename T, documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto name,
            contour::config::ConfigEntry<
                T,
                contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& v) {
            doc.append(writer.process(writer.whichDoc(v), name, v.value()));
        };

    // Something is wrong for vtpty::Process::ExecInfo formating for static build
    // we add this lambda to handle it in overload set for now
    auto const processConfigEntryWithExecInfo =
        [&]<documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>(
            auto name,
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
        [&]([[maybe_unused]] auto name, [[maybe_unused]] auto const& v) {},
    };

    // inside profiles:
    doc.append(writer.replaceCommentPlaceholder(std::string { writer.whichDoc(c.profiles) }));
    {
        const auto _ = typename Writer::Offset {};
        for (auto&& [name, entry]: c.profiles.value())
        {
            if constexpr (std::same_as<Writer, YAMLConfigWriter>)
                doc.append(std::format("    {}: \n", name));

            writer.scoped([&]() { Reflection::CallOnMembers(entry, completeOverload); });
        }
    }
    return doc;
}

template <typename Writer>
std::string createForColorScheme(Config const& c)
{
    auto doc = std::string {};
    Writer writer;

    auto const processWithDoc = [&](auto&& docString, auto... val) {
        doc.append(writer.replaceCommentPlaceholder(writer.process(writer.whichDoc(docString), val...)));
    };

    doc.append(writer.replaceCommentPlaceholder(c.colorschemes.documentation));
    writer.scoped([&]() {
        for (auto&& [name, entry]: c.colorschemes.value())
        {
            doc.append(std::format("    {}: \n", name));
            {
                const auto _ = typename Writer::Offset {};
                processWithDoc(documentation::DefaultColors {},
                               entry.defaultBackground,
                               entry.defaultForeground,
                               entry.defaultForegroundBright,
                               entry.defaultForegroundDimmed);

                // processWithDoc("# Background image support.\n"
                //                "background_image:\n"
                //                "    # Full path to the image to use as background.\n"
                //                "    #\n"
                //                "    # Default: empty string (disabled)\n"
                //                "    # path: '/Users/trapni/Pictures/bg.png'\n"
                //                "\n"
                //                "    # Image opacity to be applied to make the image not look to
                //                intense\n" "    # and not get too distracted by the background
                //                image.\n" "    opacity: {}\n"
                //                "\n"
                //                "    # Optionally blurs background image to make it less
                //                distracting\n" "    # and keep the focus on the actual terminal
                //                contents.\n" "    #\n" "    blur: {}\n",
                //                entry.backgroundImage->opacity,
                //                entry.backgroundImage->blur);

                // processWithDoc("# Mandates the color of the cursor and potentially overridden
                // text.\n"
                //                "#\n"
                //                "# The color can be specified in RGB as usual, plus\n"
                //                "# - CellForeground: Selects the cell's foreground color.\n"
                //                "# - CellBackground: Selects the cell's background color.\n"
                //                "cursor:\n"
                //                "    # Specifies the color to be used for the actual cursor
                //                shape.\n" "    #\n" "    default: {}\n" "    # Specifies the
                //                color to be used for the characters that would\n" "    # be
                //                covered otherwise.\n" "    #\n" "    text: {}\n",
                //                entry.cursor.color, entry.cursor.textOverrideColor);

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
        }
    });

    return doc;
}

template <typename Writer>
std::string createKeyMapping(Config const& c)
{
    auto doc = std::string {};
    Writer writer;

    doc.append(writer.replaceCommentPlaceholder(c.inputMappings.documentation));
    {
        const auto _ = typename Writer::Offset {};
        for (auto&& entry: c.inputMappings.value().keyMappings)
            doc.append(Writer::addOffset(writer.format(entry), Writer::Offset::levels * Writer::OneOffset));

        for (auto&& entry: c.inputMappings.value().charMappings)
            doc.append(Writer::addOffset(writer.format(entry), Writer::Offset::levels * Writer::OneOffset));

        for (auto&& entry: c.inputMappings.value().mouseMappings)
            doc.append(Writer::addOffset(writer.format(entry), Writer::Offset::levels * Writer::OneOffset));
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

} // namespace contour::config
