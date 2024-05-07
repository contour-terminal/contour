// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/Config.h>

#include <crispy/StrongHash.h>
#include <crispy/escape.h>

#include <yaml-cpp/emitter.h>

#include <QtCore/QFile>
#include <QtGui/QOpenGLContext>

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
                throw runtime_error { fmt::format(
                    "Could not create directory {}. {}", path.parent_path().string(), ec.message()) };
    }

    vector<fs::path> getTermInfoDirs(optional<fs::path> const& appTerminfoDir)
    {
        auto locations = vector<fs::path>();

        if (appTerminfoDir.has_value())
            locations.emplace_back(appTerminfoDir.value().string());

        locations.emplace_back(Process::homeDirectory() / ".terminfo");

        if (auto const* value = getenv("TERMINFO_DIRS"); value && *value)
            for (auto const dir: crispy::split(string_view(value), ':'))
                locations.emplace_back(string(dir));

        locations.emplace_back("/usr/share/terminfo");

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
        auto const terms = vector<string> {
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
                if (access((prefix / fmt::format("{:02X}", term.at(0)) / term).string().c_str(), R_OK) == 0)
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
    return createString(c, YAMLConfigWriter());
}

std::string defaultConfigString()
{
    const Config config {};
    auto configString = createString(config);

    auto logger = configLog;
    logger()(configString);

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

/**
 * @return success or failure of loading the config file.
 */
void loadConfigFromFile(Config& config, fs::path const& fileName)
{
    auto logger = configLog;
    logger()("Loading configuration from file: {} ", fileName.string());
    config.configFile = fileName;
    createFileIfNotExists(config.configFile);

    auto yamlVisitor = YAMLConfigReader(config.configFile.string(), configLog);
    yamlVisitor.load(config);
}

optional<std::string> readConfigFile(std::string const& filename)
{
    for (fs::path const& prefix: configHomes("contour"))
        if (auto text = readFile(prefix / filename); text.has_value())
            return text;

    return nullopt;
}

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
        loadFromEntry("word_delimiters", c.wordDelimiters);
        loadFromEntry("extended_word_delimiters", c.extendedWordDelimiters);
        loadFromEntry("read_buffer_size", c.ptyReadBufferSize);
        loadFromEntry("pty_buffer_size", c.ptyBufferObjectSize);
        loadFromEntry("images.sixel_register_count", c.maxImageColorRegisters);
        loadFromEntry("live_config", c.live);
        loadFromEntry("early_exit_threshold", c.earlyExitThreshold);
        loadFromEntry("spawn_new_process", c.spawnNewProcess);
        loadFromEntry("images.sixe_scrolling", c.sixelScrolling);
        loadFromEntry("reflow_on_resize", c.reflowOnResize);
        loadFromEntry("experimental", c.experimentalFeatures);
        loadFromEntry("renderer.tile_direct_mapping", c.textureAtlasDirectMapping);
        loadFromEntry("renderer.tile_hastable_slots", c.textureAtlasHashtableSlots);
        loadFromEntry("renderer.tile_cache_count", c.textureAtlasTileCount);
        loadFromEntry("bypass_mouse_protocol_modifier", c.bypassMouseProtocolModifiers);
        loadFromEntry("on_mouse_select", c.onMouseSelection);
        loadFromEntry("mouse_block_selection_modifier", c.mouseBlockSelectionModifiers);
        loadFromEntry("images", c.maxImageSize);
        loadFromEntry("profiles", c.profiles);
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
        // clang-format off
        if (child["shell"])
        {
            loadFromEntry(child, "shell", where.shell);
        }
        else if (child["ssh"])
        {
            loadFromEntry(child, "ssh", where.ssh);
        }
        else
        {
            // will create default shell if no shell nor ssh config is provided
            loadFromEntry(child, "shell", where.shell);
        }
        // inforce some default shell setup
        defaultSettings(where.shell.value());

        loadFromEntry(child, "escape_sandbox", where.shell.value().escapeSandbox);
        loadFromEntry(child, "copy_last_mark_range_offset", where.copyLastMarkRangeOffset);
        if(child["initial_working_directory"])
            {
                loadFromEntry(child, "initial_working_directory", where.shell.value().workingDirectory);
            }
        else
            {
                where.shell.value().workingDirectory = homeResolvedPath(where.shell.value().workingDirectory.generic_string(), Process::homeDirectory());
            }

        loadFromEntry(child, "show_title_bar", where.showTitleBar);
        loadFromEntry(child, "size_indicator_on_resize", where.sizeIndicatorOnResize);
        loadFromEntry(child, "fullscreen", where.fullscreen);
        loadFromEntry(child, "maximized", where.maximized);
        loadFromEntry(child, "bell", where.bell);
        loadFromEntry(child, "wm_class", where.wmClass);
        loadFromEntry(child, "margins", where.margins);
        loadFromEntry(child, "terminal_id", where.terminalId);
        loadFromEntry(child, "frozen_dec_modes", where.frozenModes);
        loadFromEntry(child, "slow_scrolling_time", where.smoothLineScrolling);
        loadFromEntry(child, "terminal_size", where.terminalSize);
        if (child["history"])
        {
            loadFromEntry(child["history"], "limit", where.maxHistoryLineCount);
            loadFromEntry(child["history"], "scroll_multiplier", where.historyScrollMultiplier);
            loadFromEntry(child["history"], "auto_scroll_on_update", where.autoScrollOnUpdate);
        }
        if (child["scrollbar"])
        {
            loadFromEntry(child["scrollbar"], "position", where.scrollbarPosition);
            loadFromEntry(child["scrollbar"], "hide_in_alt_screen", where.hideScrollbarInAltScreen);
        }
        if (child["mouse"])
            loadFromEntry(child["mouse"], "hide_while_typing", where.mouseHideWhileTyping);
        if (child["permissions"])
        {
            loadFromEntry(child["permissions"], "capture_buffer", where.captureBuffer);
            loadFromEntry(child["permissions"], "change_font", where.changeFont);
            loadFromEntry(child["permissions"],
                          "display_host_writable_statusline",
                          where.displayHostWritableStatusLine);
        }
        loadFromEntry(child, "highlight_word_and_matches_on_double_click", where.highlightDoubleClickedWord);
        loadFromEntry(child, "font", where.fonts);
        loadFromEntry(child, "draw_bold_text_with_bright_colors", where.drawBoldTextWithBrightColors);
        if (child["cursor"])
        {
            loadFromEntry(child["cursor"], "shape", where.modeInsert.value().cursor.cursorShape);
            loadFromEntry(child["cursor"], "blinking", where.modeInsert.value().cursor.cursorDisplay);
            loadFromEntry(child["cursor"], "blinking_interval", where.modeInsert.value().cursor.cursorBlinkInterval);
        }
        if (child["normal_mode"] && child["normal_mode"]["cursor"])
        {
            loadFromEntry(child["normal_mode"]["cursor"], "shape", where.modeNormal.value().cursor.cursorShape);
            loadFromEntry(child["normal_mode"]["cursor"], "blinking", where.modeNormal.value().cursor.cursorDisplay);
            loadFromEntry(child["normal_mode"]["cursor"], "blinking_interval", where.modeNormal.value().cursor.cursorBlinkInterval);
        }
        if (child["visual_mode"] && child["visual_mode"]["cursor"])
        {
            loadFromEntry(child["visual_mode"]["cursor"], "shape", where.modeVisual.value().cursor.cursorShape);
            loadFromEntry(child["visual_mode"]["cursor"], "blinking", where.modeVisual.value().cursor.cursorDisplay);
            loadFromEntry(child["visual_mode"]["cursor"], "blinking_interval", where.modeVisual.value().cursor.cursorBlinkInterval);
            loadFromEntry(child["visual_mode"]["cursor"], "blinking_interval", where.modeVisual.value().cursor.cursorBlinkInterval);
        }
        loadFromEntry(child, "vi_mode_highlight_timeout", where.highlightTimeout);
        loadFromEntry(child, "vi_mode_scrolloff", where.modalCursorScrollOff);
        if (child["status_line"])
        {
            loadFromEntry(child["status_line"], "position", where.statusDisplayPosition);
            loadFromEntry(child["status_line"], "sync_to_window_title", where.syncWindowTitleWithHostWritableStatusDisplay);
            loadFromEntry(child["status_line"], "display", where.initialStatusDisplayType);

            if (child["status_line"]["indicator"])
            {
                loadFromEntry(child["status_line"]["indicator"], "left", where.indicatorStatusLineLeft);
                loadFromEntry(child["status_line"]["indicator"], "middle", where.indicatorStatusLineMiddle);
                loadFromEntry(child["status_line"]["indicator"], "right", where.indicatorStatusLineRight);
            }
        }
        if (child["background"])
        {
            loadFromEntry(child["background"], "opacity", where.backgroundOpacity);
            loadFromEntry(child["background"], "blur", where.backgroundBlur);
        }
        // clang-format on

        loadFromEntry(child, "colors", where.colors);

        if (auto* simple = get_if<SimpleColorConfig>(&(where.colors.value())))
            simple->colors.useBrightColors = where.drawBoldTextWithBrightColors.value();
        else if (auto* dual = get_if<DualColorConfig>(&(where.colors.value())))
        {
            dual->darkMode.useBrightColors = where.drawBoldTextWithBrightColors.value();
            dual->lightMode.useBrightColors = where.drawBoldTextWithBrightColors.value();
        }

        loadFromEntry(child, "hyperlink_decoration.normal", where.hyperlinkDecorationNormal);
        loadFromEntry(child, "hyperlink_decoration.hover", where.hyperlinkDecorationHover);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::ColorPalette& where)
{
    logger()("color palette loading {}", entry);
    auto child = node[entry];

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
            return loadFromEntry(YAML::Load(fileContents.value()), where);
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
    loadWithLog("vi_mode_cursosrline", where.indicatorStatusLine);
    loadWithLog("selection", where.selection);
    loadWithLog("search_highlight", where.searchHighlight);
    loadWithLog("search_highlight_focused", where.searchHighlightFocused);
    loadWithLog("word_highlight_current", where.wordHighlightCurrent);
    loadWithLog("word_highlight_other", where.wordHighlight);
    loadWithLog("indicator_statusline", where.indicatorStatusLine);
    loadWithLog("indicator_statusline_inactive", where.indicatorStatusLineInactive);
    loadWithLog("input_method_editor", where.inputMethodEditor);

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
        loadFromEntry(child, "known_hosts", where.publicKeyFile);
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
    if (auto args = node["arguments"]; args && args.IsSequence())
    {
        for (auto const& argNode: args)
            where.arguments.emplace_back(argNode.as<string>());
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
        }
        else // entries like emoji: "emoji"
        {
            where.familyName = child.as<std::string>();
        }
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
    auto constexpr NativeFontLocator =
#if defined(_WIN32)
        vtrasterizer::FontLocatorEngine::DWrite;
#elif defined(__APPLE__)
        vtrasterizer::FontLocatorEngine::CoreText;
#else
        vtrasterizer::FontLocatorEngine::FontConfig;
#endif
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtrasterizer::FontLocatorEngine> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "fontconfig")
            return vtrasterizer::FontLocatorEngine::FontConfig;
        if (literal == "coretext")
            return vtrasterizer::FontLocatorEngine::CoreText;
        if (literal == "dwrite" || literal == "directwrite")
            return vtrasterizer::FontLocatorEngine::DWrite;
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
                        logger()(
                            "Adding input mapping: mods: {:<20} modifiers: {:<20} key: {:<20} action: {:<20}",
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
        fmt::format("{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH);
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
        return false;

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

    auto const lowerName = crispy::toUpper(name);
    for (auto const& mapping: NamedChars)
        if (lowerName == mapping.first)
            return static_cast<char32_t>(mapping.second);

    return std::nullopt;
}

std::optional<vtbackend::Key> YAMLConfigReader::parseKey(std::string const& name)
{
    using vtbackend::Key;
    using namespace std::literals::string_view_literals;
    auto static constexpr Mappings = std::array {
        std::pair { "F1"sv, Key::F1 },
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
    };

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

        if (holds_alternative<actions::WriteScreen>(action))
        {
            if (auto chars = node["chars"]; chars.IsScalar())
            {
                return actions::WriteScreen { chars.as<std::string>() };
            }
            else
                return std::nullopt;
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
std::string createString(Config const& c)
{
    auto doc = std::string {};
    Writer writer;
    auto const process = [&](auto v) {
        doc.append(writer.process(v.documentation, v.value()));
    };

    auto const processWithDoc = [&](auto&& docString, auto... val) {
        doc.append(
            fmt::format(fmt::runtime(writer.process(docString.value, val...)), fmt::arg("comment", "#")));
    };

    auto const processWordDelimiters = [&]() {
        auto wordDelimiters = c.wordDelimiters.value();
        wordDelimiters = std::regex_replace(wordDelimiters, std::regex("\\\\"), "\\$&"); /* \ -> \\ */
        wordDelimiters = std::regex_replace(wordDelimiters, std::regex("\""), "\\$&");   /* " -> \" */
        doc.append(
            fmt::format(fmt::runtime(writer.process(documentation::WordDelimiters.value, wordDelimiters)),
                        fmt::arg("comment", "#")));
    };

    auto const processExtendedWordDelimiters = [&]() {
        auto wordDelimiters = c.extendedWordDelimiters.value();
        wordDelimiters = std::regex_replace(wordDelimiters, std::regex("\\\\"), "\\$&"); /* \ -> \\ */
        wordDelimiters = std::regex_replace(wordDelimiters, std::regex("\""), "\\$&");   /* " -> \" */
        doc.append(
            fmt::format(fmt::runtime(writer.process(documentation::ExtendedWordDelimiters.value, wordDelimiters)),
                        fmt::arg("comment", "#")));
    };


    if (c.platformPlugin.value() == "")
    {
        processWithDoc(documentation::PlatformPlugin, std::string { "auto" });
    }
    else
    {
        process(c.platformPlugin);
    }

    // inside renderer:
    writer.scoped([&]() {
        doc.append("renderer: \n");
        process(c.renderingBackend);
        process(c.textureAtlasHashtableSlots);
        process(c.textureAtlasTileCount);
        process(c.textureAtlasDirectMapping);
    });

    processWordDelimiters();
    processExtendedWordDelimiters();
    process(c.ptyReadBufferSize);
    process(c.ptyBufferObjectSize);
    process(c.defaultProfileName);
    process(c.earlyExitThreshold);
    process(c.spawnNewProcess);
    process(c.reflowOnResize);
    process(c.bypassMouseProtocolModifiers);
    process(c.mouseBlockSelectionModifiers);
    process(c.onMouseSelection);
    process(c.live);
    process(c.experimentalFeatures);

    // inside images:
    doc.append("\nimages: \n");

    writer.scoped([&]() {
        process(c.sixelScrolling);
        process(c.maxImageColorRegisters);
        process(c.maxImageSize);
    });

    // inside profiles:
    doc.append(fmt::format(fmt::runtime(c.profiles.documentation), fmt::arg("comment", "#")));
    {
        const auto _ = typename Writer::Offset {};
        for (auto&& [name, entry]: c.profiles.value())
        {
            doc.append(fmt::format("    {}: \n", name));
            {
                const auto _ = typename Writer::Offset {};
                process(entry.shell);
                process(entry.ssh);

                processWithDoc(documentation::EscapeSandbox, entry.shell.value().escapeSandbox);
                process(entry.copyLastMarkRangeOffset);
                processWithDoc(documentation::InitialWorkingDirectory, [&entry = entry]() {
                    auto fromConfig = entry.shell.value().workingDirectory.string();
                    if (fromConfig.empty())
                        return std::string { "\"~\"" };
                    return fromConfig;
                }());
                process(entry.showTitleBar);
                process(entry.sizeIndicatorOnResize);
                process(entry.fullscreen);
                process(entry.maximized);
                process(entry.bell);
                process(entry.wmClass);
                process(entry.terminalId);
                processWithDoc(documentation::FrozenDecMode, 0);
                process(entry.smoothLineScrolling);
                process(entry.terminalSize);

                process(entry.margins);
                // history: section
                doc.append(Writer::addOffset("history:\n", Writer::Offset::levels * Writer::OneOffset));
                {
                    const auto _ = typename Writer::Offset {};
                    process(entry.maxHistoryLineCount);
                    process(entry.autoScrollOnUpdate);
                    process(entry.historyScrollMultiplier);
                }

                // scrollbar: section
                doc.append(Writer::addOffset("scrollbar:\n", Writer::Offset::levels * Writer::OneOffset));
                ;
                {
                    const auto _ = typename Writer::Offset {};
                    process(entry.scrollbarPosition);
                    process(entry.hideScrollbarInAltScreen);
                }

                // mouse: section
                doc.append(Writer::addOffset("mouse:\n", Writer::Offset::levels * Writer::OneOffset));
                {
                    const auto _ = typename Writer::Offset {};
                    process(entry.mouseHideWhileTyping);
                }

                //  permissions: section
                processWithDoc(
                    documentation::StringLiteral {
                        "\n"
                        "{comment} Some VT sequences should need access permissions.\n"
                        "{comment} \n"
                        "{comment}  These can be to:\n"
                        "{comment}  - allow     Allows the given functionality\n"
                        "{comment}  - deny      Denies the given functionality\n"
                        "{comment}  - ask       Asks the user interactively via popup dialog for "
                        "permission of the given action.\n"
                        "permissions:\n" },
                    0);
                {
                    const auto _ = typename Writer::Offset {};
                    process(entry.changeFont);
                    process(entry.captureBuffer);
                    process(entry.displayHostWritableStatusLine);
                }
                process(entry.highlightDoubleClickedWord);
                process(entry.fonts);
                process(entry.drawBoldTextWithBrightColors);
                process(entry.modeInsert);
                process(entry.modeNormal);
                process(entry.modeVisual);
                process(entry.highlightTimeout);
                process(entry.modalCursorScrollOff);

                // status_line
                doc.append(Writer::addOffset("\n"
                                             "status_line:\n",
                                             Writer::Offset::levels * Writer::OneOffset));
                {
                    const auto _ = typename Writer::Offset {};
                    process(entry.initialStatusDisplayType);
                    process(entry.statusDisplayPosition);
                    process(entry.syncWindowTitleWithHostWritableStatusDisplay);

                    doc.append(Writer::addOffset("indicator:\n", Writer::Offset::levels * Writer::OneOffset));
                    {
                        const auto _ = typename Writer::Offset {};
                        process(entry.indicatorStatusLineLeft);
                        process(entry.indicatorStatusLineMiddle);
                        process(entry.indicatorStatusLineRight);
                    }
                }

                doc.append(Writer::addOffset("\n"
                                             "background:\n",
                                             Writer::Offset::levels * Writer::OneOffset));
                {
                    const auto _ = typename Writer::Offset {};
                    process(entry.backgroundOpacity);
                    process(entry.backgroundBlur);
                }

                process(entry.colors);

                doc.append(Writer::addOffset("\n"
                                             "hyperlink_decoration:\n",
                                             Writer::Offset::levels * Writer::OneOffset));
                {
                    const auto _ = typename Writer::Offset {};
                    process(entry.hyperlinkDecorationNormal);
                    process(entry.hyperlinkDecorationHover);
                }
            }
        };
    }

    doc.append(fmt::format(fmt::runtime(c.colorschemes.documentation), fmt::arg("comment", "#")));
    writer.scoped([&]() {
        for (auto&& [name, entry]: c.colorschemes.value())
        {
            doc.append(fmt::format("    {}: \n", name));

            {
                const auto _ = typename Writer::Offset {};
                doc.append(
                    fmt::format(fmt::runtime(Writer::addOffset("{comment} Default colors\n"
                                                               "default:\n",
                                                               Writer::Offset::levels * Writer::OneOffset)),
                                fmt::arg("comment", "#")));
                {
                    const auto _ = typename Writer::Offset {};
                    processWithDoc(
                        documentation::DefaultColors, entry.defaultBackground, entry.defaultForeground);

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

                    processWithDoc(documentation::HyperlinkDecoration,
                                   entry.hyperlinkDecoration.normal,
                                   entry.hyperlinkDecoration.hover);

                    processWithDoc(documentation::YankHighlight,
                                   entry.yankHighlight.foreground,
                                   entry.yankHighlight.foregroundAlpha,
                                   entry.yankHighlight.background,
                                   entry.yankHighlight.backgroundAlpha);

                    processWithDoc(documentation::NormalModeCursorline,
                                   entry.normalModeCursorline.foreground,
                                   entry.normalModeCursorline.foregroundAlpha,
                                   entry.normalModeCursorline.background,
                                   entry.normalModeCursorline.backgroundAlpha);

                    processWithDoc(documentation::Selection,
                                   entry.selection.foreground,
                                   entry.selection.foregroundAlpha,
                                   entry.selection.background,
                                   entry.selection.backgroundAlpha);

                    processWithDoc(documentation::SearchHighlight,
                                   entry.searchHighlight.foreground,
                                   entry.searchHighlight.foregroundAlpha,
                                   entry.searchHighlight.background,
                                   entry.searchHighlight.backgroundAlpha);

                    processWithDoc(documentation::SearchHighlihtFocused,
                                   entry.searchHighlightFocused.foreground,
                                   entry.searchHighlightFocused.foregroundAlpha,
                                   entry.searchHighlightFocused.background,
                                   entry.searchHighlightFocused.backgroundAlpha);

                    processWithDoc(documentation::WordHighlightCurrent,
                                   entry.wordHighlightCurrent.foreground,
                                   entry.wordHighlightCurrent.foregroundAlpha,
                                   entry.wordHighlightCurrent.background,
                                   entry.wordHighlightCurrent.backgroundAlpha);

                    processWithDoc(documentation::WordHighlight,
                                   entry.wordHighlight.foreground,
                                   entry.wordHighlight.foregroundAlpha,
                                   entry.wordHighlight.background,
                                   entry.wordHighlight.backgroundAlpha);

                    processWithDoc(documentation::IndicatorStatusLine,
                                   entry.indicatorStatusLine.foreground,
                                   entry.indicatorStatusLine.background);

                    processWithDoc(documentation::IndicatorStatusLineInactive,
                                   entry.indicatorStatusLineInactive.foreground,
                                   entry.indicatorStatusLineInactive.background);

                    processWithDoc(documentation::InputMethodEditor,
                                   entry.inputMethodEditor.foreground,
                                   entry.inputMethodEditor.background);

                    processWithDoc(documentation::NormalColors,
                                   entry.normalColor(0),
                                   entry.normalColor(1),
                                   entry.normalColor(2),
                                   entry.normalColor(3),
                                   entry.normalColor(4),
                                   entry.normalColor(5),
                                   entry.normalColor(6),
                                   entry.normalColor(7));

                    processWithDoc(documentation::BrightColors,
                                   entry.brightColor(0),
                                   entry.brightColor(1),
                                   entry.brightColor(2),
                                   entry.brightColor(3),
                                   entry.brightColor(4),
                                   entry.brightColor(5),
                                   entry.brightColor(6),
                                   entry.brightColor(7));

                    processWithDoc(documentation::DimColors,
                                   entry.dimColor(0),
                                   entry.dimColor(1),
                                   entry.dimColor(2),
                                   entry.dimColor(3),
                                   entry.dimColor(4),
                                   entry.dimColor(5),
                                   entry.dimColor(6),
                                   entry.dimColor(7));
                }

                doc.append(Writer::addOffset("", Writer::Offset::levels * Writer::OneOffset));
            }
        }
    });

    doc.append(fmt::format(fmt::runtime(c.inputMappings.documentation), fmt::arg("comment", "#")));
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

} // namespace contour::config
