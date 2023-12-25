// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/Config.h>

#include <vtbackend/ColorPalette.h>
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/TerminalState.h>
#include <vtbackend/primitives.h>

#include <vtpty/Process.h>

#include <text_shaper/mock_font_locator.h>

#include <crispy/StrongHash.h>
#include <crispy/escape.h>
#include <crispy/logstore.h>
#include <crispy/overloaded.h>
#include <crispy/utils.h>

#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

#include <QtCore/QFile>
#include <QtGui/QOpenGLContext>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

    string processIdAsString()
    {
        // There's sadly no better way to platfrom-independantly get the PID.
        auto stringStream = std::stringstream();
#if defined(_WIN32)
        stringStream << static_cast<unsigned>(GetCurrentProcessId());
#else
        stringStream << getpid();
#endif
        return stringStream.str();
    }

    struct VariableReplacer
    {
        auto operator()(string_view name) -> string
        {
            if (name == "pid")
                return processIdAsString();
            return ""s;
        }
    };

    std::shared_ptr<vtbackend::BackgroundImage const> loadImage(string const& fileName,
                                                                float opacity,
                                                                bool blur)
    {
        auto const resolvedFileName = homeResolvedPath(fileName, Process::homeDirectory());

        if (!fs::exists(resolvedFileName))
        {
            errorLog()("Background image path not found: {}", resolvedFileName.string());
            return nullptr;
        }

        auto backgroundImage = vtbackend::BackgroundImage {};
        backgroundImage.location = resolvedFileName;
        backgroundImage.hash = crispy::strong_hash::compute(resolvedFileName.string());
        backgroundImage.opacity = opacity;
        backgroundImage.blur = blur;

        return make_shared<vtbackend::BackgroundImage const>(std::move(backgroundImage));
    }

    vtbackend::CellRGBColor parseCellColor(std::string const& text)
    {
        auto const upperText = toUpper(text);
        if (upperText == "CELLBACKGROUND"sv)
            return vtbackend::CellBackgroundColor {};
        if (upperText == "CELLFOREGROUND"sv)
            return vtbackend::CellForegroundColor {};
        return vtbackend::RGBColor(text);
    }

    vtbackend::CellRGBColor parseCellColor(UsedKeys& usedKeys,
                                           YAML::Node const& parentNode,
                                           std::string const& parentPath,
                                           std::string const& name,
                                           vtbackend::CellRGBColor defaultValue)
    {
        auto colorNode = parentNode[name];
        if (!colorNode || !colorNode.IsScalar())
            return defaultValue;
        usedKeys.emplace(parentPath + "." + name);
        return parseCellColor(colorNode.as<string>());
    }

    std::optional<vtbackend::RGBColorPair> parseRGBColorPair(UsedKeys& usedKeys,
                                                             string const& basePath,
                                                             YAML::Node const& baseNode,
                                                             string const& childNodeName,
                                                             vtbackend::RGBColorPair defaultPair)
    {
        auto node = baseNode[childNodeName];
        if (!node || !node.IsMap())
            return nullopt;

        auto const childPath = fmt::format("{}.{}", basePath, childNodeName);
        usedKeys.emplace(childPath);

        auto rgbColorPair = defaultPair;

        if (auto const value = node["foreground"]; value && value.IsScalar())
        {
            rgbColorPair.foreground = value.as<string>();
            usedKeys.emplace(childPath + ".foreground");
        }

        if (auto const value = node["background"]; value && value.IsScalar())
        {
            rgbColorPair.background = value.as<string>();
            usedKeys.emplace(childPath + ".background");
        }

        return rgbColorPair;
    }

    /// Loads a configuration sub-section to handle cell color foreground/background + alpha.
    ///
    /// Example:
    ///   { foreground: CellColor, foreground_alpha: FLOAT = 1.0,
    ///     background: CellColor, background_alpha: FLOAT = 1.0 }
    std::optional<CellRGBColorAndAlphaPair> parseCellRGBColorAndAlphaPair(UsedKeys& usedKeys,
                                                                          string const& basePath,
                                                                          YAML::Node const& baseNode,
                                                                          string const& childNodeName)
    {
        auto node = baseNode[childNodeName];
        if (!node)
            return nullopt;

        auto const childPath = fmt::format("{}.{}", basePath, childNodeName);
        usedKeys.emplace(childPath);

        auto cellRGBColorAndAlphaPair = CellRGBColorAndAlphaPair {};

        cellRGBColorAndAlphaPair.foreground =
            parseCellColor(usedKeys, node, childPath, "foreground", vtbackend::CellForegroundColor {});
        if (auto alpha = node["foreground_alpha"]; alpha && alpha.IsScalar())
        {
            usedKeys.emplace(childPath + ".foreground_alpha");
            cellRGBColorAndAlphaPair.foregroundAlpha = std::clamp(alpha.as<float>(), 0.0f, 1.0f);
        }

        cellRGBColorAndAlphaPair.background =
            parseCellColor(usedKeys, node, childPath, "background", vtbackend::CellBackgroundColor {});
        if (auto alpha = node["background_alpha"]; alpha && alpha.IsScalar())
        {
            usedKeys.emplace(childPath + ".background_alpha");
            cellRGBColorAndAlphaPair.backgroundAlpha = std::clamp(alpha.as<float>(), 0.0f, 1.0f);
        }

        return cellRGBColorAndAlphaPair;
    }

    // TODO:
    // - [x] report missing keys
    // - [ ] report superfluous keys (by keeping track of loaded keys, then iterate
    //       through full document and report any key that has not been loaded but is available)
    // - [ ] Do we want to report when no color schemes are defined? (at least warn about?)
    // - [ ] Do we want to report when no input mappings are defined? (at least warn about?)

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

    optional<Permission> toPermission(string const& value)
    {
        auto const lowerValue = toLower(value);
        if (lowerValue == "allow")
            return Permission::Allow;
        else if (lowerValue == "deny")
            return Permission::Deny;
        else if (lowerValue == "ask")
            return Permission::Ask;
        return nullopt;
    }

    void createFileIfNotExists(fs::path const& path)
    {
        if (!fs::is_regular_file(path))
            if (auto const ec = createDefaultConfig(path); ec)
                throw runtime_error { fmt::format(
                    "Could not create directory {}. {}", path.parent_path().string(), ec.message()) };
    }

    template <typename T>
    bool tryLoadValueRelative(UsedKeys& usedKeys,
                              YAML::Node const& currentNode,
                              string const& basePath,
                              vector<string_view> const& keys,
                              size_t offset,
                              T& store,
                              logstore::message_builder const& logger)
    {
        string parentKey = basePath;
        for (size_t i = 0; i < offset; ++i)
        {
            if (!parentKey.empty())
                parentKey += '.';
            parentKey += keys.at(i);
        }

        if (offset == keys.size())
        {
            store = currentNode.as<T>();
            return true;
        }

        auto const currentKey = string(keys.at(offset));

        auto const child = currentNode[currentKey];
        if (!child)
        {
            auto const defaultStr = crispy::escape(fmt::format("{}", store));
            auto const defaultStrQuoted = !defaultStr.empty() ? defaultStr : R"("")";
            for (size_t i = offset; i < keys.size(); ++i)
            {
                if (!parentKey.empty())
                    parentKey += '.';
                parentKey += keys[i];
            }
            logger("Missing key {}. Using default: {}.", parentKey, defaultStrQuoted);
            return false;
        }

        usedKeys.emplace(parentKey);

        return tryLoadValueRelative(usedKeys, child, keys, offset + 1, store, logger);
    }

    template <typename T>
    bool tryLoadValue(UsedKeys& usedKeys,
                      YAML::Node const& root,
                      vector<string_view> const& keys,
                      size_t offset,
                      T& store,
                      logstore::category logger)
    {
        string parentKey;
        for (size_t i = 0; i < offset; ++i)
        {
            if (i)
                parentKey += '.';
            parentKey += keys.at(i);
        }

        if (offset == keys.size())
        {
            store = root.as<T>();
            return true;
        }

        auto const currentKey = string(keys.at(offset));

        auto const child = root[currentKey];
        if (!child)
        {
            auto const defaultStr = crispy::escape(fmt::format("{}", store));
            for (size_t i = offset; i < keys.size(); ++i)
            {
                parentKey += '.';
                parentKey += keys[i];
            }
            logger()(
                "Missing key {}. Using default: {}.", parentKey, !defaultStr.empty() ? defaultStr : R"("")");
            return false;
        }

        usedKeys.emplace(parentKey);

        return tryLoadValue(usedKeys, child, keys, offset + 1, store, logger);
    }

    template <typename T, typename U>
    bool tryLoadValue(UsedKeys& usedKeys,
                      YAML::Node const& root,
                      vector<string_view> const& keys,
                      size_t offset,
                      boxed::boxed<T, U>& store,
                      logstore::category const& logger)
    {
        return tryLoadValue(usedKeys, root, keys, offset, store.value, logger);
    }

    template <typename T>
    bool tryLoadValue(UsedKeys& usedKeys,
                      YAML::Node const& root,
                      string const& path,
                      T& store,
                      logstore::category const& logger)
    {
        auto const keys = crispy::split(path, '.');
        usedKeys.emplace(path);
        return tryLoadValue(usedKeys, root, keys, 0, store, logger);
    }

    template <typename T, typename U>
    bool tryLoadValue(UsedKeys& usedKeys,
                      YAML::Node const& root,
                      string const& path,
                      boxed::boxed<T, U>& store,
                      logstore::category const& logger)
    {
        return tryLoadValue(usedKeys, root, path, store.value, logger);
    }

    template <typename T>
    bool tryLoadChild(UsedKeys& usedKeys,
                      YAML::Node const& doc,
                      string const& parentPath,
                      string const& key,
                      T& store,
                      logstore::category const& logger)
    {
        auto const path = fmt::format("{}.{}", parentPath, key);
        return tryLoadValue(usedKeys, doc, path, store, logger);
    }

    template <typename T>
    bool tryLoadChildRelative(UsedKeys& usedKeys,
                              YAML::Node const& node,
                              string const& parentPath,
                              string const& childKeyPath,
                              T& store,
                              logstore::category const& logger)
    {
        // return tryLoadValue(usedKeys, node, childKeyPath, store); // XXX parentPath
        auto const keys = crispy::split(childKeyPath, '.');
        string s = parentPath;
        for (auto const key: keys)
        {
            s += fmt::format(".{}", key);
            usedKeys.emplace(s);
        }
        return tryLoadValue(usedKeys, node, keys, 0, store, logger);
    }

    template <typename T, typename U>
    bool tryLoadChild(UsedKeys& usedKeys,
                      YAML::Node const& doc,
                      string const& parentPath,
                      string const& key,
                      boxed::boxed<T, U>& store,
                      logstore::category const& logger)
    {
        return tryLoadChild(usedKeys, doc, parentPath, key, store.value, logger);
    }

    void checkForSuperfluousKeys(YAML::Node root, string const& thePrefix, UsedKeys const& usedKeys)
    {
        if (root.IsMap())
        {
            for (auto const& mapItem: root)
            {
                auto const name = mapItem.first.as<string>();
                auto const child = mapItem.second;
                auto const prefix = thePrefix.empty() ? name : fmt::format("{}.{}", thePrefix, name);
                checkForSuperfluousKeys(child, prefix, usedKeys);
                if (usedKeys.count(prefix))
                    continue;
                if (crispy::startsWith(string_view(prefix), "x-"sv))
                    continue;
                configLog()("Superfluous config key found: {}", escape(prefix));
            }
        }
        else if (root.IsSequence())
        {
            for (size_t i = 0; i < root.size() && i < 8; ++i)
            {
                checkForSuperfluousKeys(root[i], fmt::format("{}.{}", thePrefix, i), usedKeys);
            }
        }
#if 0
        else if (root.IsScalar())
        {
        }
        else if (root.IsNull())
        {
            ; // no-op
        }
#endif
    }

    void checkForSuperfluousKeys(YAML::Node const& root, UsedKeys const& usedKeys)
    {
        checkForSuperfluousKeys(root, "", usedKeys);
    }

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

    optional<vtbackend::Key> parseKey(string const& name)
    {
        using vtbackend::Key;
        auto static constexpr Mappings = array {
            pair { "F1"sv, Key::F1 },
            pair { "F2"sv, Key::F2 },
            pair { "F3"sv, Key::F3 },
            pair { "F4"sv, Key::F4 },
            pair { "F5"sv, Key::F5 },
            pair { "F6"sv, Key::F6 },
            pair { "F7"sv, Key::F7 },
            pair { "F8"sv, Key::F8 },
            pair { "F9"sv, Key::F9 },
            pair { "F10"sv, Key::F10 },
            pair { "F11"sv, Key::F11 },
            pair { "F12"sv, Key::F12 },
            pair { "F13"sv, Key::F13 },
            pair { "F14"sv, Key::F14 },
            pair { "F15"sv, Key::F15 },
            pair { "F16"sv, Key::F16 },
            pair { "F17"sv, Key::F17 },
            pair { "F18"sv, Key::F18 },
            pair { "F19"sv, Key::F19 },
            pair { "F20"sv, Key::F20 },
            pair { "F21"sv, Key::F21 },
            pair { "F22"sv, Key::F22 },
            pair { "F23"sv, Key::F23 },
            pair { "F24"sv, Key::F24 },
            pair { "F25"sv, Key::F25 },
            pair { "F26"sv, Key::F26 },
            pair { "F27"sv, Key::F27 },
            pair { "F28"sv, Key::F28 },
            pair { "F29"sv, Key::F29 },
            pair { "F30"sv, Key::F30 },
            pair { "F31"sv, Key::F31 },
            pair { "F32"sv, Key::F32 },
            pair { "F33"sv, Key::F33 },
            pair { "F34"sv, Key::F34 },
            pair { "F35"sv, Key::F35 },
            pair { "Escape"sv, Key::Escape },
            pair { "Enter"sv, Key::Enter },
            pair { "Tab"sv, Key::Tab },
            pair { "Backspace"sv, Key::Backspace },
            pair { "DownArrow"sv, Key::DownArrow },
            pair { "LeftArrow"sv, Key::LeftArrow },
            pair { "RightArrow"sv, Key::RightArrow },
            pair { "UpArrow"sv, Key::UpArrow },
            pair { "Insert"sv, Key::Insert },
            pair { "Delete"sv, Key::Delete },
            pair { "Home"sv, Key::Home },
            pair { "End"sv, Key::End },
            pair { "PageUp"sv, Key::PageUp },
            pair { "PageDown"sv, Key::PageDown },
            pair { "MediaPlay"sv, Key::MediaPlay },
            pair { "MediaStop"sv, Key::MediaStop },
            pair { "MediaPrevious"sv, Key::MediaPrevious },
            pair { "MediaNext"sv, Key::MediaNext },
            pair { "MediaPause"sv, Key::MediaPause },
            pair { "MediaTogglePlayPause"sv, Key::MediaTogglePlayPause },
            pair { "VolumeUp"sv, Key::VolumeUp },
            pair { "VolumeDown"sv, Key::VolumeDown },
            pair { "VolumeMute"sv, Key::VolumeMute },
            pair { "PrintScreen"sv, Key::PrintScreen },
            pair { "Pause"sv, Key::Pause },
            pair { "Menu"sv, Key::Menu },
            pair { "Numpad_0"sv, Key::Numpad_0 },
            pair { "Numpad_1"sv, Key::Numpad_1 },
            pair { "Numpad_2"sv, Key::Numpad_2 },
            pair { "Numpad_3"sv, Key::Numpad_3 },
            pair { "Numpad_4"sv, Key::Numpad_4 },
            pair { "Numpad_5"sv, Key::Numpad_5 },
            pair { "Numpad_6"sv, Key::Numpad_6 },
            pair { "Numpad_7"sv, Key::Numpad_7 },
            pair { "Numpad_8"sv, Key::Numpad_8 },
            pair { "Numpad_9"sv, Key::Numpad_9 },
            pair { "Numpad_Decimal"sv, Key::Numpad_Decimal },
            pair { "Numpad_Divide"sv, Key::Numpad_Divide },
            pair { "Numpad_Multiply"sv, Key::Numpad_Multiply },
            pair { "Numpad_Subtract"sv, Key::Numpad_Subtract },
            pair { "Numpad_Add"sv, Key::Numpad_Add },
            pair { "Numpad_Enter"sv, Key::Numpad_Enter },
            pair { "Numpad_Equal"sv, Key::Numpad_Equal },
        };

        auto const lowerName = toLower(name);

        for (auto const& mapping: Mappings)
            if (lowerName == toLower(mapping.first))
                return mapping.second;

        return nullopt;
    }

    optional<variant<vtbackend::Key, char32_t>> parseKeyOrChar(string const& name)
    {
        using namespace vtbackend::ControlCode;

        if (auto const key = parseKey(name); key.has_value())
            return key.value();

        auto const text = QString::fromUtf8(name.c_str()).toUcs4();
        if (text.size() == 1)
            return static_cast<char32_t>(text[0]);

        auto constexpr NamedChars = array {
            pair { "LESS"sv, '<' },        pair { "GREATER"sv, '>' },      pair { "PLUS"sv, '+' },
            pair { "APOSTROPHE"sv, '\'' }, pair { "ADD"sv, '+' },          pair { "BACKSLASH"sv, 'x' },
            pair { "COMMA"sv, ',' },       pair { "DECIMAL"sv, '.' },      pair { "DIVIDE"sv, '/' },
            pair { "EQUAL"sv, '=' },       pair { "LEFT_BRACKET"sv, '[' }, pair { "MINUS"sv, '-' },
            pair { "MULTIPLY"sv, '*' },    pair { "PERIOD"sv, '.' },       pair { "RIGHT_BRACKET"sv, ']' },
            pair { "SEMICOLON"sv, ';' },   pair { "SLASH"sv, '/' },        pair { "SUBTRACT"sv, '-' },
            pair { "SPACE"sv, ' ' },
        };

        auto const lowerName = toUpper(name);
        for (auto const& mapping: NamedChars)
            if (lowerName == mapping.first)
                return static_cast<char32_t>(mapping.second);

        return nullopt;
    }

    void parseCursorConfig(CursorConfig& cursorConfig,
                           YAML::Node const& rootNode,
                           UsedKeys& usedKeys,
                           std::string const& basePath)
    {
        if (!rootNode)
            return;

        std::string strValue;
        tryLoadChildRelative(usedKeys, rootNode, basePath, "shape", strValue, configLog);
        if (!strValue.empty())
            cursorConfig.cursorShape = vtbackend::makeCursorShape(strValue);

        bool boolValue = cursorConfig.cursorDisplay == vtbackend::CursorDisplay::Blink;
        tryLoadChildRelative(usedKeys, rootNode, basePath, "blinking", boolValue, configLog);
        cursorConfig.cursorDisplay =
            boolValue ? vtbackend::CursorDisplay::Blink : vtbackend::CursorDisplay::Steady;

        auto uintValue = cursorConfig.cursorBlinkInterval.count();
        tryLoadChildRelative(usedKeys, rootNode, basePath, "blinking_interval", uintValue, configLog);
        cursorConfig.cursorBlinkInterval = chrono::milliseconds(uintValue);
    }

    optional<vtbackend::Modifier> parseModifierKey(string const& key)
    {
        using vtbackend::Modifier;
        auto const upperKey = toUpper(key);
        if (upperKey == "ALT")
            return Modifier::Alt;
        if (upperKey == "CONTROL")
            return Modifier::Control;
        if (upperKey == "SHIFT")
            return Modifier::Shift;
        if (upperKey == "SUPER")
            return Modifier::Super;
        if (upperKey == "COMMAND")
            // This represents the Command key on macOS, which is equivalent to the Windows key on Windows,
            // and the Super key on Linux.
            return Modifier::Super;
        if (upperKey == "META")
            // TODO: This is technically not correct, but we used the term Meta up until now,
            // to refer to the Windows/Cmd key. But Qt also exposes another modifier called
            // Meta, which rarely exists on modern keyboards (?), but it we need to support it
            // as well, especially since extended CSIu protocol exposes it as well.
            return Modifier::Super; // Return Modifier::Meta in the future.
        return nullopt;
    }

    optional<vtbackend::MatchModes> parseMatchModes(UsedKeys& usedKeys,
                                                    string const& prefix,
                                                    YAML::Node const& node)
    {
        using vtbackend::MatchModes;
        if (!node)
            return vtbackend::MatchModes {};
        usedKeys.emplace(prefix);
        if (!node.IsScalar())
            return nullopt;

        auto matchModes = MatchModes {};

        auto const modeStr = node.as<string>();
        auto const args = crispy::split(modeStr, '|');
        for (string_view arg: args)
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
            string const upperArg = toUpper(arg);
            if (upperArg == "ALT"sv)
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

    optional<vtbackend::Modifiers> parseModifiers(UsedKeys& usedKeys,
                                                  string const& prefix,
                                                  YAML::Node const& node)
    {
        using vtbackend::Modifier;
        using vtbackend::Modifiers;
        if (!node)
            return nullopt;
        usedKeys.emplace(prefix);
        if (node.IsScalar())
            return parseModifierKey(node.as<string>());
        if (!node.IsSequence())
            return nullopt;

        vtbackend::Modifiers mods;
        for (const auto& i: node)
        {
            if (!i.IsScalar())
                return nullopt;

            auto const mod = parseModifierKey(i.as<string>());
            if (!mod)
                return nullopt;

            mods |= *mod;
        }
        return mods;
    }

    template <typename Input>
    void appendOrCreateBinding(vector<vtbackend::InputBinding<Input, ActionList>>& bindings,
                               vtbackend::MatchModes modes,
                               vtbackend::Modifiers modifiers,
                               Input input,
                               Action action)
    {
        for (auto& binding: bindings)
        {
            if (match(binding, modes, modifiers, input))
            {
                binding.binding.emplace_back(std::move(action));
                return;
            }
        }

        bindings.emplace_back(vtbackend::InputBinding<Input, ActionList> {
            modes, modifiers, input, ActionList { std::move(action) } });
    }

    bool tryAddKey(InputMappings& inputMappings,
                   vtbackend::MatchModes modes,
                   vtbackend::Modifiers modifiers,
                   YAML::Node const& node,
                   Action action)
    {
        if (!node)
            return false;

        if (!node.IsScalar())
            return false;

        auto const input = parseKeyOrChar(node.as<string>());
        if (!input.has_value())
            return false;

        if (holds_alternative<vtbackend::Key>(*input))
        {
            appendOrCreateBinding(
                inputMappings.keyMappings, modes, modifiers, get<vtbackend::Key>(*input), std::move(action));
        }
        else if (holds_alternative<char32_t>(*input))
        {
            appendOrCreateBinding(
                inputMappings.charMappings, modes, modifiers, get<char32_t>(*input), std::move(action));
        }
        else
            assert(false && "The impossible happened.");

        return true;
    }

    optional<vtbackend::MouseButton> parseMouseButton(YAML::Node const& node)
    {
        if (!node)
            return nullopt;

        if (!node.IsScalar())
            return nullopt;

        auto constexpr static Mappings = array {
            pair { "WHEELUP"sv, vtbackend::MouseButton::WheelUp },
            pair { "WHEELDOWN"sv, vtbackend::MouseButton::WheelDown },
            pair { "LEFT"sv, vtbackend::MouseButton::Left },
            pair { "MIDDLE"sv, vtbackend::MouseButton::Middle },
            pair { "RIGHT"sv, vtbackend::MouseButton::Right },
        };
        auto const upperName = toUpper(node.as<string>());
        for (auto const& mapping: Mappings)
            if (upperName == mapping.first)
                return mapping.second;
        return nullopt;
    }

    bool tryAddMouse(vector<MouseInputMapping>& bindings,
                     vtbackend::MatchModes modes,
                     vtbackend::Modifiers modifiers,
                     YAML::Node const& node,
                     Action action)
    {
        auto mouseButton = parseMouseButton(node);
        if (!mouseButton)
            return false;

        appendOrCreateBinding(bindings, modes, modifiers, *mouseButton, std::move(action));
        return true;
    }

    optional<Action> parseAction(UsedKeys& usedKeys, string const& prefix, YAML::Node const& parent)
    {
        usedKeys.emplace(prefix + ".action");

        auto actionName = parent["action"].as<string>();
        usedKeys.emplace(prefix + ".action." + actionName);
        auto actionOpt = actions::fromString(actionName);
        if (!actionOpt)
        {
            errorLog()("Unknown action '{}'.", parent["action"].as<string>());
            return nullopt;
        }

        auto action = actionOpt.value();

        if (holds_alternative<actions::ChangeProfile>(action))
        {
            if (auto name = parent["name"]; name.IsScalar())
            {
                usedKeys.emplace(prefix + ".name");
                return actions::ChangeProfile { name.as<string>() };
            }
            else
                return nullopt;
        }

        if (holds_alternative<actions::NewTerminal>(action))
        {
            if (auto profile = parent["profile"]; profile && profile.IsScalar())
            {
                usedKeys.emplace(prefix + ".profile");
                return actions::NewTerminal { profile.as<string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::ReloadConfig>(action))
        {
            usedKeys.emplace(prefix + ".profile");
            if (auto profileName = parent["profile"]; profileName.IsScalar())
            {
                usedKeys.emplace(prefix + ".profile");
                return actions::ReloadConfig { profileName.as<string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::SendChars>(action))
        {
            if (auto chars = parent["chars"]; chars.IsScalar())
            {
                usedKeys.emplace(prefix + ".chars");
                return actions::SendChars { unescape(chars.as<string>()) };
            }
            else
                return nullopt;
        }

        if (holds_alternative<actions::CopySelection>(action))
        {
            if (auto node = parent["format"]; node && node.IsScalar())
            {
                usedKeys.emplace(prefix + ".format");
                auto const formatString = toUpper(node.as<string>());
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
                errorLog()("Invalid format '{}' in CopySelection action. Defaulting to 'text'.",
                           node.as<string>());
                return actions::CopySelection { actions::CopyFormat::Text };
            }
        }

        if (holds_alternative<actions::PasteClipboard>(action))
        {
            if (auto node = parent["strip"]; node && node.IsScalar())
            {
                usedKeys.emplace(prefix + ".strip");
                return actions::PasteClipboard { node.as<bool>() };
            }
        }

        if (holds_alternative<actions::WriteScreen>(action))
        {
            if (auto chars = parent["chars"]; chars.IsScalar())
            {
                usedKeys.emplace(prefix + ".chars");
                return actions::WriteScreen { unescape(chars.as<string>()) };
            }
            else
                return nullopt;
        }

        return action;
    }

    void parseInputMapping(UsedKeys& usedKeys,
                           string const& prefix,
                           Config& config,
                           YAML::Node const& mapping)
    {
        using namespace vtbackend;

        auto const action = parseAction(usedKeys, prefix, mapping);
        auto const mods = parseModifiers(usedKeys, prefix + ".mods", mapping["mods"]);
        auto const mode = parseMatchModes(usedKeys, prefix + ".mode", mapping["mode"]);
        if (action && mods && mode)
        {
            if (tryAddKey(config.inputMappings, *mode, *mods, mapping["key"], *action))
            {
                usedKeys.emplace(prefix + ".key");
            }
            else if (tryAddMouse(config.inputMappings.mouseMappings, *mode, *mods, mapping["mouse"], *action))
            {
                usedKeys.emplace(prefix + ".mouse");
            }
            else
            {
                // TODO: log error: invalid key mapping at: mapping.sourceLocation()
                configLog()("Could not add some input mapping.");
            }
        }
    }

    void updateColorScheme(vtbackend::ColorPalette& colors,
                           UsedKeys& usedKeys,
                           string const& basePath,
                           YAML::Node const& node)

    {
        if (!node)
            return;

        usedKeys.emplace(basePath);
        using vtbackend::RGBColor;
        if (auto def = node["default"]; def)
        {
            usedKeys.emplace(basePath + ".default");
            if (auto fg = def["foreground"]; fg)
            {
                usedKeys.emplace(basePath + ".default.foreground");
                colors.defaultForeground = fg.as<string>();
            }
            if (auto bg = def["background"]; bg)
            {
                usedKeys.emplace(basePath + ".default.background");
                colors.defaultBackground = bg.as<string>();
            }
        }

        if (auto p = parseCellRGBColorAndAlphaPair(usedKeys, basePath, node, "search_highlight"))
            colors.searchHighlight = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(usedKeys, basePath, node, "search_highlight_focused"))
            colors.searchHighlightFocused = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(usedKeys, basePath, node, "word_highlight_current"))
            colors.wordHighlightCurrent = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(usedKeys, basePath, node, "word_highlight_other"))
            colors.wordHighlight = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(usedKeys, basePath, node, "selection"))
            colors.selection = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(usedKeys, basePath, node, "vi_mode_highlight"))
            colors.yankHighlight = p.value();

        if (auto p = parseCellRGBColorAndAlphaPair(usedKeys, basePath, node, "vi_mode_cursorline"))
            colors.normalModeCursorline = p.value();

        if (auto p = parseRGBColorPair(
                usedKeys, basePath, node, "indicator_statusline", colors.indicatorStatusLine))
            colors.indicatorStatusLine = p.value();

        if (auto p = parseRGBColorPair(usedKeys,
                                       basePath,
                                       node,
                                       "indicator_statusline_inactive",
                                       colors.indicatorStatusLineInactive))
            colors.indicatorStatusLineInactive = p.value();

        if (auto const p =
                parseRGBColorPair(usedKeys, basePath, node, "input_method_editor", colors.inputMethodEditor))
            colors.inputMethodEditor = p.value();

        if (auto cursor = node["cursor"]; cursor)
        {
            usedKeys.emplace(basePath + ".cursor");
            if (cursor.IsMap())
            {
                if (auto color = cursor["default"]; color.IsScalar())
                {
                    usedKeys.emplace(basePath + ".cursor.default");
                    colors.cursor.color = parseCellColor(color.as<string>());
                }
                if (auto color = cursor["text"]; color.IsScalar())
                {
                    usedKeys.emplace(basePath + ".cursor.text");
                    colors.cursor.textOverrideColor = parseCellColor(color.as<string>());
                }
            }
            else if (cursor.IsScalar())
            {
                errorLog()(
                    "Deprecated cursor config colorscheme entry. Please update your colorscheme entry for "
                    "cursor.");
                colors.cursor.color = RGBColor(cursor.as<string>());
            }
            else
                errorLog()("Invalid cursor config colorscheme entry.");
        }

        if (auto hyperlink = node["hyperlink_decoration"]; hyperlink)
        {
            usedKeys.emplace(basePath + ".hyperlink_decoration");
            if (auto color = hyperlink["normal"]; color && color.IsScalar() && !color.as<string>().empty())
            {
                usedKeys.emplace(basePath + ".hyperlink_decoration.normal");
                colors.hyperlinkDecoration.normal = color.as<string>();
            }

            if (auto color = hyperlink["hover"]; color && color.IsScalar() && !color.as<string>().empty())
            {
                usedKeys.emplace(basePath + ".hyperlink_decoration.hover");
                colors.hyperlinkDecoration.hover = color.as<string>();
            }
        }

        auto const loadColorMap = [&](YAML::Node const& parent, string const& key, size_t offset) -> bool {
            auto node = parent[key];
            if (!node)
                return false;

            auto const colorKeyPath = fmt::format("{}.{}", basePath, key);
            usedKeys.emplace(colorKeyPath);
            if (node.IsMap())
            {
                auto const assignColor = [&](size_t index, string const& name) {
                    if (auto nodeValue = node[name]; nodeValue)
                    {
                        usedKeys.emplace(fmt::format("{}.{}", colorKeyPath, name));
                        if (auto const value = nodeValue.as<string>(); !value.empty())
                        {
                            if (value[0] == '#')
                                colors.palette[offset + index] = value;
                            else if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
                                colors.palette[offset + index] = RGBColor { nodeValue.as<uint32_t>() };
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
                        colors.palette[i] = RGBColor { node[i].as<uint32_t>() };
                    else
                        colors.palette[i] = RGBColor { node[i].as<string>() };
                return true;
            }
            return false;
        };

        loadColorMap(node, "normal", 0);
        loadColorMap(node, "bright", 8);
        if (!loadColorMap(node, "dim", 256))
        {
            // calculate dim colors based on normal colors
            for (unsigned i = 0; i < 8; ++i)
                colors.palette[256 + i] = colors.palette[i] * 0.5f;
        }

        // TODO: color palette from 16..255

        float opacityValue = 1.0;
        tryLoadChildRelative(usedKeys, node, basePath, "background_image.opacity", opacityValue, configLog);

        bool imageBlur = false;
        tryLoadChildRelative(usedKeys, node, basePath, "background_image.blur", imageBlur, configLog);

        string fileName;
        if (tryLoadChildRelative(usedKeys, node, basePath, "background_image.path", fileName, configLog))
            colors.backgroundImage = loadImage(fileName, opacityValue, imageBlur);
    }

    vtbackend::ColorPalette loadColorScheme(UsedKeys& usedKeys,
                                            string const& basePath,
                                            YAML::Node const& node)
    {

        vtbackend::ColorPalette colors;
        updateColorScheme(colors, usedKeys, basePath, node);
        return colors;
    }

    vtbackend::ColorPalette loadColorSchemeByName(
        UsedKeys& usedKeys,
        string const& path,
        YAML::Node const& nameNode,
        unordered_map<string, vtbackend::ColorPalette> const& colorschemes,
        logstore::category& logger)
    {
        auto const name = nameNode.as<string>();
        if (auto i = colorschemes.find(name); i != colorschemes.end())
        {
            usedKeys.emplace(path);
            return i->second;
        }

        for (fs::path const& prefix: configHomes("contour"))
        {
            auto const filePath = prefix / "colorschemes" / (name + ".yml");
            auto fileContents = readFile(filePath);
            if (!fileContents)
                continue;
            YAML::Node subDocument = YAML::Load(fileContents.value());
            UsedKeys usedColorKeys;
            auto colors = loadColorScheme(usedColorKeys, "", subDocument);
            // TODO: Check usedColorKeys for validity.
            logger()("Loaded colors from {}.", filePath.string());
            return colors;
        }
        logger()("Could not open colorscheme file for \"{}\".", name);
        return vtbackend::ColorPalette {};
    }

    void softLoadFont(UsedKeys& usedKeys,
                      string_view basePath,
                      YAML::Node const& node,
                      text::font_description& store)
    {
        if (node.IsScalar())
        {
            store.familyName = node.as<string>();
            usedKeys.emplace(basePath);
        }
        else if (node.IsMap())
        {
            usedKeys.emplace(basePath);

            if (node["family"].IsScalar())
            {
                usedKeys.emplace(fmt::format("{}.{}", basePath, "family"));
                store.familyName = node["family"].as<string>();
            }

            if (node["slant"] && node["slant"].IsScalar())
            {
                usedKeys.emplace(fmt::format("{}.{}", basePath, "slant"));
                if (auto const p = text::make_font_slant(node["slant"].as<string>()))
                    store.slant = p.value();
            }

            if (node["weight"] && node["weight"].IsScalar())
            {
                usedKeys.emplace(fmt::format("{}.{}", basePath, "weight"));
                if (auto const p = text::make_font_weight(node["weight"].as<string>()))
                    store.weight = p.value();
            }

            if (node["features"] && node["features"] && node["features"].IsSequence())
            {
                usedKeys.emplace(fmt::format("{}.{}", basePath, "features"));
                YAML::Node featuresNode = node["features"];
                for (auto&& i: featuresNode)
                {
                    auto const featureNode = i;
                    if (!featureNode.IsScalar())
                    {
                        errorLog()("Invalid font feature \"{}\".", featureNode.as<string>());
                        continue;
                    }

                    // Feature can be either 4 letter code or optionally ending with - to denote disabling it.
                    auto const [tag, enabled] = [&]() -> tuple<string, bool> {
                        auto value = featureNode.as<string>();
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
                        errorLog()(
                            "Invalid font feature \"{}\". Font features are denoted as 4-letter codes.",
                            featureNode.as<string>());
                        continue;
                    }
                    store.features.emplace_back(tag[0], tag[1], tag[2], tag[3], enabled);
                }
            }
        }
    }

    void softLoadFont(vtrasterizer::TextShapingEngine textShapingEngine,
                      UsedKeys& usedKeys,
                      string_view basePath,
                      YAML::Node const& parentNode,
                      string const& key,
                      text::font_description& store)
    {
        if (!parentNode)
            return;
        auto node = parentNode[key];
        if (!node)
            return;

        softLoadFont(usedKeys, fmt::format("{}.{}", basePath, key), node, store);

        if (node.IsMap())
        {
            usedKeys.emplace(fmt::format("{}.{}", basePath, key));
            if (node["features"].IsSequence())
            {
                using vtrasterizer::TextShapingEngine;
                switch (textShapingEngine)
                {
                    case TextShapingEngine::OpenShaper: break;
                    case TextShapingEngine::CoreText:
                    case TextShapingEngine::DWrite:
                        // TODO: Implement font feature settings handling for these engines.
                        errorLog()("The configured text shaping engine {} does not yet support font feature "
                                   "settings. Ignoring.",
                                   textShapingEngine);
                }
            }
        }
    }

    template <typename T>
    bool sanitizeRange(std::reference_wrapper<T> value, T min, T max)
    {
        if (min <= value.get() && value.get() <= max)
            return true;

        value.get() = std::clamp(value.get(), min, max);
        return false;
    }

    optional<vtbackend::VTType> stringToVTType(std::string const& value)
    {
        using Type = vtbackend::VTType;
        auto constexpr static Mappings = array<tuple<string_view, Type>, 10> {
            tuple { "VT100"sv, Type::VT100 }, tuple { "VT220"sv, Type::VT220 },
            tuple { "VT240"sv, Type::VT240 }, tuple { "VT330"sv, Type::VT330 },
            tuple { "VT340"sv, Type::VT340 }, tuple { "VT320"sv, Type::VT320 },
            tuple { "VT420"sv, Type::VT420 }, tuple { "VT510"sv, Type::VT510 },
            tuple { "VT520"sv, Type::VT520 }, tuple { "VT525"sv, Type::VT525 }
        };
        for (auto const& mapping: Mappings)
            if (get<0>(mapping) == value)
                return get<1>(mapping);
        return nullopt;
    }

    void updateTerminalProfile(TerminalProfile& terminalProfile,
                               UsedKeys& usedKeys,
                               YAML::Node const& profile,
                               std::string const& parentPath,
                               std::string const& profileName,
                               unordered_map<string, vtbackend::ColorPalette> const& colorschemes,
                               logstore::category logger)
    {
        if (auto colors = profile["colors"]; colors) // {{{
        {
            usedKeys.emplace(fmt::format("{}.{}.colors", parentPath, profileName));
            auto const path = fmt::format("{}.{}.{}", parentPath, profileName, "colors");
            if (colors.IsMap())
            {
                terminalProfile.colors =
                    DualColorConfig { .darkMode = loadColorSchemeByName(
                                          usedKeys, path + ".dark", colors["dark"], colorschemes, logger),
                                      .lightMode = loadColorSchemeByName(
                                          usedKeys, path + ".light", colors["light"], colorschemes, logger) };
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
                errorLog()("Dual color scheme is not supported by your local Qt version. "
                           "Falling back to single color scheme.");
#endif
            }
            else if (colors.IsScalar())
            {
                terminalProfile.colors =
                    SimpleColorConfig { loadColorSchemeByName(usedKeys, path, colors, colorschemes, logger) };
            }
            else
                logger()("Invalid colors value.");
        }
        else
            logger()("No colors section in profile {} found.", profileName);
        // }}}

        string const basePath = fmt::format("{}.{}", parentPath, profileName);
        tryLoadChildRelative(
            usedKeys, profile, basePath, "escape_sandbox", terminalProfile.shell.escapeSandbox, logger);
        tryLoadChildRelative(usedKeys, profile, basePath, "shell", terminalProfile.shell.program, logger);
        if (terminalProfile.shell.program.empty())
        {
            if (!terminalProfile.shell.arguments.empty())
                logger()("No shell defined but arguments. Ignoring arguments.");

            auto loginShell = Process::loginShell(terminalProfile.shell.escapeSandbox);
            terminalProfile.shell.program = loginShell.front();
            loginShell.erase(loginShell.begin());
            terminalProfile.shell.arguments = loginShell;
        }
#if defined(VTPTY_LIBSSH2)
        if (auto const sshNode = profile["ssh"]; sshNode && sshNode.IsMap())
        {
            usedKeys.emplace(fmt::format("{}.{}.ssh", parentPath, profileName));
            auto& ssh = terminalProfile.ssh;
            std::string strValue;
            tryLoadChildRelative(usedKeys, profile, basePath, "ssh.host", ssh.hostname, logger);

            // Get SSH config from ~/.ssh/config
            if (!ssh.hostname.empty())
            {
                if (auto const sshConfigResult = vtpty::loadSshConfig(); sshConfigResult)
                {
                    // NB: We don't care if we could not load the ~/.ssh/config, we then simply don't use it
                    auto const& sshConfig = sshConfigResult.value();
                    if (auto const i = sshConfig.find(ssh.hostname); i != sshConfig.end())
                    {
                        configLog()("Using SSH config for host \"{}\" as base. {}",
                                    ssh.hostname,
                                    i->second.toString());
                        ssh = i->second;
                    }
                }
            }

            // Override with values from profile
            tryLoadChildRelative(usedKeys, profile, basePath, "ssh.port", ssh.port, logger);
            tryLoadChildRelative(usedKeys, profile, basePath, "ssh.forward_agent", ssh.forwardAgent, logger);
            if (!tryLoadChildRelative(usedKeys, profile, basePath, "ssh.user", ssh.username, logger)
                && ssh.username.empty())
                ssh.username = vtpty::Process::userName();
            if (tryLoadChildRelative(usedKeys, profile, basePath, "ssh.private_key", strValue, logger))
                ssh.privateKeyFile = homeResolvedPath(strValue, Process::homeDirectory());
            if (tryLoadChildRelative(usedKeys, profile, basePath, "ssh.public_key", strValue, logger))
                ssh.publicKeyFile = homeResolvedPath(strValue, Process::homeDirectory());
            if (tryLoadChildRelative(usedKeys, profile, basePath, "ssh.known_hosts", strValue, logger))
                ssh.knownHostsFile = homeResolvedPath(strValue, Process::homeDirectory());
            else if (auto const p = Process::homeDirectory() / ".ssh" / "known_hosts"; fs::exists(p))
                ssh.knownHostsFile = p;
        }
#endif

        tryLoadChildRelative(usedKeys, profile, basePath, "maximized", terminalProfile.maximized, logger);
        tryLoadChildRelative(usedKeys, profile, basePath, "fullscreen", terminalProfile.fullscreen, logger);
        tryLoadChildRelative(
            usedKeys, profile, basePath, "refresh_rate", terminalProfile.refreshRate.value, logger);
        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "copy_last_mark_range_offset",
                             terminalProfile.copyLastMarkRangeOffset,
                             logger);
        tryLoadChildRelative(
            usedKeys, profile, basePath, "show_title_bar", terminalProfile.showTitleBar, logger);
        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "size_indicator_on_resize",
                             terminalProfile.sizeIndicatorOnResize,
                             logger);
        bool useBrightColors = false;
        tryLoadChildRelative(
            usedKeys, profile, basePath, "draw_bold_text_with_bright_colors", useBrightColors, logger);

        if (auto* simple = get_if<SimpleColorConfig>(&terminalProfile.colors))
            simple->colors.useBrightColors = useBrightColors;
        else if (auto* dual = get_if<DualColorConfig>(&terminalProfile.colors))
        {
            dual->darkMode.useBrightColors = useBrightColors;
            dual->lightMode.useBrightColors = useBrightColors;
        }

        tryLoadChildRelative(usedKeys, profile, basePath, "wm_class", terminalProfile.wmClass, logger);

        if (auto args = profile["arguments"]; args && args.IsSequence())
        {
            usedKeys.emplace(fmt::format("{}.arguments", basePath));
            for (auto const& argNode: args)
                terminalProfile.shell.arguments.emplace_back(argNode.as<string>());
        }

        std::string strValue;
        tryLoadChildRelative(usedKeys, profile, basePath, "initial_working_directory", strValue, logger);
        if (!strValue.empty())
            terminalProfile.shell.workingDirectory = fs::path(strValue);

        terminalProfile.shell.workingDirectory = homeResolvedPath(
            terminalProfile.shell.workingDirectory.generic_string(), Process::homeDirectory());

        terminalProfile.shell.env["TERMINAL_NAME"] = "contour";
        terminalProfile.shell.env["TERMINAL_VERSION_TRIPLE"] =
            fmt::format("{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH);
        terminalProfile.shell.env["TERMINAL_VERSION_STRING"] = CONTOUR_VERSION_STRING;

        // {{{ Populate environment variables
        std::optional<fs::path> appTerminfoDir;
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
                    terminalProfile.shell.env["TERMINFO_DIRS"] = p.string();
                }
            }
        }
#endif

        if (auto env = profile["environment"]; env)
        {
            auto const envpath = basePath + ".environment";
            usedKeys.emplace(envpath);
            for (auto i = env.begin(); i != env.end(); ++i)
            {
                auto const name = i->first.as<string>();
                auto const value = i->second.as<string>();
                usedKeys.emplace(fmt::format("{}.{}", envpath, name));
                terminalProfile.shell.env[name] = value;
            }
        }

        // force some default env
        if (terminalProfile.shell.env.find("TERM") == terminalProfile.shell.env.end())
        {
            terminalProfile.shell.env["TERM"] = getDefaultTERM(appTerminfoDir);
            logger()("Defaulting TERM to {}.", terminalProfile.shell.env["TERM"]);
        }

        if (terminalProfile.shell.env.find("COLORTERM") == terminalProfile.shell.env.end())
            terminalProfile.shell.env["COLORTERM"] = "truecolor";

            // This is currently duplicated. Environment vars belong to each parent struct, not just shell.
#if defined(VTPTY_LIBSSH2)
        terminalProfile.ssh.env = terminalProfile.shell.env;
#endif
        // }}}

        strValue = fmt::format("{}", terminalProfile.terminalId);
        tryLoadChildRelative(usedKeys, profile, basePath, "terminal_id", strValue, logger);
        if (auto const idOpt = stringToVTType(strValue))
            terminalProfile.terminalId = idOpt.value();
        else
            logger()("Invalid Terminal ID \"{}\", specified", strValue);

        tryLoadChildRelative(
            usedKeys, profile, basePath, "margins.horizontal", terminalProfile.margins.horizontal, logger);
        tryLoadChildRelative(
            usedKeys, profile, basePath, "margins.vertical", terminalProfile.margins.vertical, logger);

        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "terminal_size.columns",
                             terminalProfile.terminalSize.columns,
                             logger);
        tryLoadChildRelative(
            usedKeys, profile, basePath, "terminal_size.lines", terminalProfile.terminalSize.lines, logger);
        {
            auto constexpr MinimalTerminalSize = PageSize { LineCount(3), ColumnCount(3) };
            auto constexpr MaximumTerminalSize = PageSize { LineCount(200), ColumnCount(300) };

            if (!sanitizeRange(ref(terminalProfile.terminalSize.columns.value),
                               *MinimalTerminalSize.columns,
                               *MaximumTerminalSize.columns))
                logger()("Terminal width {} out of bounds. Should be between {} and {}.",
                         terminalProfile.terminalSize.columns,
                         MinimalTerminalSize.columns,
                         MaximumTerminalSize.columns);

            if (!sanitizeRange(ref(terminalProfile.terminalSize.lines),
                               MinimalTerminalSize.lines,
                               MaximumTerminalSize.lines))
                logger()("Terminal height {} out of bounds. Should be between {} and {}.",
                         terminalProfile.terminalSize.lines,
                         MinimalTerminalSize.lines,
                         MaximumTerminalSize.lines);
        }

        strValue = "ask";
        if (tryLoadChildRelative(usedKeys, profile, basePath, "permissions.capture_buffer", strValue, logger))
        {
            if (auto x = toPermission(strValue))
                terminalProfile.permissions.captureBuffer = x.value();
        }

        strValue = "ask";
        if (tryLoadChildRelative(usedKeys, profile, basePath, "permissions.change_font", strValue, logger))
        {
            if (auto x = toPermission(strValue))
                terminalProfile.permissions.changeFont = x.value();
        }

        strValue = "ask";
        if (tryLoadChildRelative(usedKeys,
                                 profile,
                                 basePath,
                                 "permissions.display_host_writable_statusline",
                                 strValue,
                                 logger))
        {
            if (auto x = toPermission(strValue))
                terminalProfile.permissions.displayHostWritableStatusLine = x.value();
        }

        if (tryLoadChildRelative(
                usedKeys, profile, basePath, "font.size", terminalProfile.fonts.size.pt, logger))
        {
            if (terminalProfile.fonts.size < MinimumFontSize)
            {
                logger()("Invalid font size {} set in config file. Minimum value is {}.",
                         terminalProfile.fonts.size,
                         MinimumFontSize);
                terminalProfile.fonts.size = MinimumFontSize;
            }
        }

        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "font.builtin_box_drawing",
                             terminalProfile.fonts.builtinBoxDrawing,
                             logger);

        auto constexpr NativeTextShapingEngine =
#if defined(_WIN32)
            vtrasterizer::TextShapingEngine::DWrite;
#elif defined(__APPLE__)
            vtrasterizer::TextShapingEngine::CoreText;
#else
            vtrasterizer::TextShapingEngine::OpenShaper;
#endif

        auto constexpr NativeFontLocator =
#if defined(_WIN32)
            vtrasterizer::FontLocatorEngine::DWrite;
#elif defined(__APPLE__)
            vtrasterizer::FontLocatorEngine::CoreText;
#else
            vtrasterizer::FontLocatorEngine::FontConfig;
#endif

        strValue = fmt::format("{}", terminalProfile.fonts.textShapingEngine);
        if (tryLoadChildRelative(usedKeys, profile, basePath, "font.text_shaping.engine", strValue, logger))
        {
            auto const lwrValue = toLower(strValue);
            if (lwrValue == "dwrite" || lwrValue == "directwrite")
                terminalProfile.fonts.textShapingEngine = vtrasterizer::TextShapingEngine::DWrite;
            else if (lwrValue == "core" || lwrValue == "coretext")
                terminalProfile.fonts.textShapingEngine = vtrasterizer::TextShapingEngine::CoreText;
            else if (lwrValue == "open" || lwrValue == "openshaper")
                terminalProfile.fonts.textShapingEngine = vtrasterizer::TextShapingEngine::OpenShaper;
            else if (lwrValue == "native")
                terminalProfile.fonts.textShapingEngine = NativeTextShapingEngine;
            else
                logger()("Invalid value for configuration key {}.font.text_shaping.engine: {}",
                         basePath,
                         strValue);
        }

        terminalProfile.fonts.fontLocator = NativeFontLocator;
        strValue = fmt::format("{}", terminalProfile.fonts.fontLocator);
        if (tryLoadChildRelative(usedKeys, profile, basePath, "font.locator", strValue, logger))
        {
            auto const lwrValue = toLower(strValue);
            if (lwrValue == "fontconfig")
                terminalProfile.fonts.fontLocator = vtrasterizer::FontLocatorEngine::FontConfig;
            else if (lwrValue == "coretext")
                terminalProfile.fonts.fontLocator = vtrasterizer::FontLocatorEngine::CoreText;
            else if (lwrValue == "dwrite" || lwrValue == "directwrite")
                terminalProfile.fonts.fontLocator = vtrasterizer::FontLocatorEngine::DWrite;
            else if (lwrValue == "native")
                terminalProfile.fonts.fontLocator = NativeFontLocator;
            else if (lwrValue == "mock")
                terminalProfile.fonts.fontLocator = vtrasterizer::FontLocatorEngine::Mock;
            else
                logger()("Invalid value for configuration key {}.font.locator: {}", basePath, strValue);
        }

        bool strictSpacing = false;
        tryLoadChildRelative(usedKeys, profile, basePath, "font.strict_spacing", strictSpacing, logger);

        auto const fontBasePath = fmt::format("{}.{}.font", parentPath, profileName);

        softLoadFont(terminalProfile.fonts.textShapingEngine,
                     usedKeys,
                     fontBasePath,
                     profile["font"],
                     "regular",
                     terminalProfile.fonts.regular);

        terminalProfile.fonts.bold = terminalProfile.fonts.regular;
        terminalProfile.fonts.bold.weight = text::font_weight::bold;
        softLoadFont(terminalProfile.fonts.textShapingEngine,
                     usedKeys,
                     fontBasePath,
                     profile["font"],
                     "bold",
                     terminalProfile.fonts.bold);

        terminalProfile.fonts.italic = terminalProfile.fonts.regular;
        terminalProfile.fonts.italic.slant = text::font_slant::italic;
        softLoadFont(terminalProfile.fonts.textShapingEngine,
                     usedKeys,
                     fontBasePath,
                     profile["font"],
                     "italic",
                     terminalProfile.fonts.italic);

        terminalProfile.fonts.boldItalic = terminalProfile.fonts.regular;
        terminalProfile.fonts.boldItalic.weight = text::font_weight::bold;
        terminalProfile.fonts.boldItalic.slant = text::font_slant::italic;
        softLoadFont(terminalProfile.fonts.textShapingEngine,
                     usedKeys,
                     fontBasePath,
                     profile["font"],
                     "bold_italic",
                     terminalProfile.fonts.boldItalic);

        terminalProfile.fonts.emoji.familyName = "emoji";
        terminalProfile.fonts.emoji.spacing = text::font_spacing::mono;
        softLoadFont(terminalProfile.fonts.textShapingEngine,
                     usedKeys,
                     fontBasePath,
                     profile["font"],
                     "emoji",
                     terminalProfile.fonts.emoji);

#if defined(_WIN32)
        // Windows does not understand font family "emoji", but fontconfig does. Rewrite user-input here.
        if (terminalProfile.fonts.emoji.familyName == "emoji")
            terminalProfile.fonts.emoji.familyName = "Segoe UI Emoji";
#endif

        strValue = "gray";
        tryLoadChildRelative(usedKeys, profile, basePath, "font.render_mode", strValue, logger);
        auto const static renderModeMap = array {
            pair { "lcd"sv, text::render_mode::lcd },           pair { "light"sv, text::render_mode::light },
            pair { "gray"sv, text::render_mode::gray },         pair { ""sv, text::render_mode::gray },
            pair { "monochrome"sv, text::render_mode::bitmap },
        };

        // NOLINTNEXTLINE(readability-qualified-auto)
        if (auto const i = crispy::find_if(renderModeMap, [&](auto m) { return m.first == strValue; });
            i != renderModeMap.end())
            terminalProfile.fonts.renderMode = i->second;
        else
            logger()("Invalid render_mode \"{}\" in configuration.", strValue);

        auto intValue = LineCount();
        tryLoadChildRelative(usedKeys, profile, basePath, "history.limit", intValue, logger);
        // value -1 is used for infinite grid
        if (unbox(intValue) == -1)
            terminalProfile.maxHistoryLineCount = Infinite();
        else if (unbox(intValue) > -1)
            terminalProfile.maxHistoryLineCount = LineCount(intValue);
        else
            terminalProfile.maxHistoryLineCount = LineCount(0);

        tryLoadChildRelative(
            usedKeys, profile, basePath, "option_as_alt", terminalProfile.optionKeyAsAlt, logger);

        strValue = fmt::format("{}", ScrollBarPosition::Right);
        if (tryLoadChildRelative(usedKeys, profile, basePath, "scrollbar.position", strValue, logger))
        {
            auto const literal = toLower(strValue);
            if (literal == "left")
                terminalProfile.scrollbarPosition = ScrollBarPosition::Left;
            else if (literal == "right")
                terminalProfile.scrollbarPosition = ScrollBarPosition::Right;
            else if (literal == "hidden")
                terminalProfile.scrollbarPosition = ScrollBarPosition::Hidden;
            else
                logger()("Invalid value for config entry {}: {}", "scrollbar.position", strValue);
        }
        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "scrollbar.hide_in_alt_screen",
                             terminalProfile.hideScrollbarInAltScreen,
                             logger);

        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "mouse.hide_while_typing",
                             terminalProfile.mouseHideWhileTyping,
                             logger);

        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "history.auto_scroll_on_update",
                             terminalProfile.autoScrollOnUpdate,
                             logger);
        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "history.scroll_multiplier",
                             terminalProfile.historyScrollMultiplier,
                             logger);

        float floatValue = 1.0;
        tryLoadChildRelative(usedKeys, profile, basePath, "background.opacity", floatValue, logger);
        terminalProfile.backgroundOpacity =
            (vtbackend::Opacity)(static_cast<unsigned>(255 * clamp(floatValue, 0.0f, 1.0f)));
        tryLoadChildRelative(
            usedKeys, profile, basePath, "background.blur", terminalProfile.backgroundBlur, logger);

        strValue = "dotted-underline"; // TODO: fmt::format("{}", profile.hyperlinkDecoration.normal);
        tryLoadChildRelative(usedKeys, profile, basePath, "hyperlink_decoration.normal", strValue, logger);
        if (auto const pdeco = vtrasterizer::to_decorator(strValue); pdeco.has_value())
            terminalProfile.hyperlinkDecoration.normal = *pdeco;

        strValue = "underline"; // TODO: fmt::format("{}", profile.hyperlinkDecoration.hover);
        tryLoadChildRelative(usedKeys, profile, basePath, "hyperlink_decoration.hover", strValue, logger);

        tryLoadChildRelative(
            usedKeys, profile, basePath, "vi_mode_scrolloff", terminalProfile.modalCursorScrollOff, logger);

        auto uintValue = terminalProfile.highlightTimeout.count();
        tryLoadChildRelative(usedKeys, profile, basePath, "vi_mode_highlight_timeout", uintValue, logger);
        terminalProfile.highlightTimeout = chrono::milliseconds(uintValue);
        if (auto const pdeco = vtrasterizer::to_decorator(strValue); pdeco.has_value())
            terminalProfile.hyperlinkDecoration.hover = *pdeco;

        tryLoadChildRelative(usedKeys,
                             profile,
                             basePath,
                             "highlight_word_and_matches_on_double_click",
                             terminalProfile.highlightDoubleClickedWord,
                             logger);

        parseCursorConfig(
            terminalProfile.inputModes.insert.cursor, profile["cursor"], usedKeys, basePath + ".cursor");
        usedKeys.emplace(basePath + ".cursor");

        if (auto normalModeNode = profile["normal_mode"])
        {
            usedKeys.emplace(basePath + ".normal_mode");
            parseCursorConfig(terminalProfile.inputModes.normal.cursor,
                              normalModeNode["cursor"],
                              usedKeys,
                              basePath + ".normal_mode.cursor");
            usedKeys.emplace(basePath + ".normal_mode.cursor");
        }

        if (auto visualModeNode = profile["visual_mode"])
        {
            usedKeys.emplace(basePath + ".visual_mode");
            parseCursorConfig(terminalProfile.inputModes.visual.cursor,
                              visualModeNode["cursor"],
                              usedKeys,
                              basePath + ".visual_mode.cursor");
            usedKeys.emplace(basePath + ".visual_mode.cursor");
        }

        strValue = "none";
        tryLoadChildRelative(usedKeys, profile, basePath, "status_line.display", strValue, logger);
        if (strValue == "indicator")
            terminalProfile.initialStatusDisplayType = vtbackend::StatusDisplayType::Indicator;
        else if (strValue == "none")
            terminalProfile.initialStatusDisplayType = vtbackend::StatusDisplayType::None;
        else
            logger()("Invalid value for config entry {}: {}", "status_line.display", strValue);

        if (tryLoadChildRelative(usedKeys, profile, basePath, "status_line.position", strValue, logger))
        {
            auto const literal = toLower(strValue);
            if (literal == "bottom")
                terminalProfile.statusDisplayPosition = vtbackend::StatusDisplayPosition::Bottom;
            else if (literal == "top")
                terminalProfile.statusDisplayPosition = vtbackend::StatusDisplayPosition::Top;
            else
                logger()("Invalid value for config entry {}: {}", "status_line.position", strValue);
        }

        bool boolValue = false;
        if (tryLoadChildRelative(
                usedKeys, profile, basePath, "status_line.sync_to_window_title", boolValue, logger))
            terminalProfile.syncWindowTitleWithHostWritableStatusDisplay = boolValue;

        strValue = "default";
        if (tryLoadChildRelative(usedKeys, profile, basePath, "bell.sound", strValue, logger))
        {
            if (!strValue.empty())
            {
                if (strValue != "off" && strValue != "default")
                    strValue = "file:" + strValue;
                terminalProfile.bell.sound = strValue;
            }
        }

        boolValue = false;
        if (tryLoadChildRelative(usedKeys, profile, basePath, "bell.alert", boolValue, logger))
            terminalProfile.bell.alert = boolValue;

        floatValue = 0.0;
        if (tryLoadChildRelative(usedKeys, profile, basePath, "bell.volume", floatValue, logger))
            terminalProfile.bell.volume = std::clamp(floatValue, 0.0f, 1.0f);

        if (auto value = profile["slow_scrolling_time"])
        {
            usedKeys.emplace(basePath + ".slow_scrolling_time");
            if (value.IsScalar())
            {
                auto time = value.as<unsigned>();
                if (time < 10 || time > 2000)
                {
                    logger()(
                        "slow_scrolling_time must be between 10 and 2000 milliseconds. Defaulting to 100");
                    time = 100;
                }
                terminalProfile.smoothLineScrolling = chrono::milliseconds(time);
            }
            else
                logger()("Invalid value for config entry {}", "slow_scrolling_time");
        }

        if (auto frozenDecModes = profile["frozen_dec_modes"]; frozenDecModes)
        {
            usedKeys.emplace(basePath + ".frozen_dec_modes");
            if (frozenDecModes.IsMap())
            {
                for (auto const& modeNode: frozenDecModes)
                {
                    auto const modeNumber = std::stoi(modeNode.first.as<string>());
                    if (!vtbackend::isValidDECMode(modeNumber))
                    {
                        errorLog()("Invalid frozen_dec_modes entry: {} (Invalid DEC mode number).",
                                   modeNumber);
                        continue;
                    }
                    auto const mode = static_cast<vtbackend::DECMode>(modeNumber);
                    auto const frozenState = modeNode.second.as<bool>();
                    terminalProfile.frozenModes[mode] = frozenState;
                }
            }
            else
                errorLog()("Invalid frozen_dec_modes entry.");
        }
    }

    TerminalProfile loadTerminalProfile(UsedKeys& usedKeys,
                                        YAML::Node const& profile,
                                        std::string const& parentPath,
                                        std::string const& profileName,
                                        unordered_map<string, vtbackend::ColorPalette> const& colorschemes)
    {
        auto terminalProfile = TerminalProfile {}; // default profile
        updateTerminalProfile(
            terminalProfile, usedKeys, profile, parentPath, profileName, colorschemes, configLog);
        return terminalProfile;
    }

} // namespace
// }}}

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

std::string defaultConfigString()
{
    QFile file(":/contour/contour.yml");
    file.open(QFile::ReadOnly);
    return file.readAll().toStdString();
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
    config.backingFilePath = fileName;
    createFileIfNotExists(config.backingFilePath);
    auto usedKeys = UsedKeys {};
    YAML::Node doc;
    try
    {
        doc = YAML::LoadFile(fileName.string());
    }
    catch (exception const& e)
    {
        errorLog()("Configuration file is corrupted. {}", e.what());
        auto newfileName = fileName;
        newfileName.replace_filename("default_contour.yml");
        createDefaultConfig(newfileName);
        return loadConfigFromFile(config, newfileName);
    }
    tryLoadValue(usedKeys, doc, "word_delimiters", config.wordDelimiters, logger);

    if (auto opt =
            parseModifiers(usedKeys, "bypass_mouse_protocol_modifier", doc["bypass_mouse_protocol_modifier"]);
        opt.has_value())
        config.bypassMouseProtocolModifiers = opt.value();

    if (auto opt =
            parseModifiers(usedKeys, "mouse_block_selection_modifier", doc["mouse_block_selection_modifier"]);
        opt.has_value())
        config.mouseBlockSelectionModifiers = opt.value();

    if (doc["on_mouse_select"].IsDefined())
    {
        usedKeys.emplace("on_mouse_select");
        auto const value = toUpper(doc["on_mouse_select"].as<string>());
        auto constexpr Mappings = array {
            pair { "COPYTOCLIPBOARD", SelectionAction::CopyToClipboard },
            pair { "COPYTOSELECTIONCLIPBOARD", SelectionAction::CopyToSelectionClipboard },
            pair { "NOTHING", SelectionAction::Nothing },
        };
        bool found = false;
        for (auto const& mapping: Mappings)
            if (mapping.first == value)
            {
                config.onMouseSelection = mapping.second;
                usedKeys.emplace("on_mouse_select");
                found = true;
                break;
            }
        if (!found)
            errorLog()("Invalid action specified for on_mouse_select: {}.", value);
    }

    auto constexpr KnownExperimentalFeatures = array<string_view, 0> {
        // "tcap"sv
    };

    if (auto experimental = doc["experimental"]; experimental.IsMap())
    {
        usedKeys.emplace("experimental");
        for (auto const& x: experimental)
        {
            auto const key = x.first.as<string>();
            if (crispy::count(KnownExperimentalFeatures, key) == 0)
            {
                errorLog()("Unknown experimental feature tag: {}.", key);
                continue;
            }

            usedKeys.emplace("experimental." + x.first.as<string>());
            if (!x.second.as<bool>())
                continue;

            errorLog()("Enabling experimental feature {}.", key);
            config.experimentalFeatures.insert(key);
        }
    }

    tryLoadValue(usedKeys, doc, "spawn_new_process", config.spawnNewProcess, logger);

    tryLoadValue(usedKeys, doc, "live_config", config.live, logger);

    if (auto const loggingNode = doc["logging"]; loggingNode && loggingNode.IsMap())
    {
        usedKeys.emplace("logging");

        if (auto const tagsNode = loggingNode["tags"]; tagsNode && tagsNode.IsSequence())
        {
            usedKeys.emplace("logging.tags");
            for (auto const& tagNode: tagsNode)
            {
                auto const tag = tagNode.as<string>();
                usedKeys.emplace("logging.tags." + tag);
                logstore::enable(tag);
            }
        }
    }

    auto logEnabled = false;
    tryLoadValue(usedKeys, doc, "logging.enabled", logEnabled, logger);

    auto logFilePath = ""s;
    tryLoadValue(usedKeys, doc, "logging.file", logFilePath, logger);

    if (logEnabled)
    {
        logFilePath =
            homeResolvedPath(replaceVariables(logFilePath, VariableReplacer()), Process::homeDirectory())
                .generic_string();

        if (!logFilePath.empty())
        {
            config.loggingSink = make_shared<logstore::sink>(logEnabled, make_shared<ofstream>(logFilePath));
            logstore::set_sink(*config.loggingSink);
        }
    }

    tryLoadValue(usedKeys, doc, "images.sixel_scrolling", config.sixelScrolling, logger);
    tryLoadValue(usedKeys, doc, "images.sixel_register_count", config.maxImageColorRegisters, logger);
    tryLoadValue(usedKeys, doc, "images.max_width", config.maxImageSize.width, logger);
    tryLoadValue(usedKeys, doc, "images.max_height", config.maxImageSize.height, logger);

    if (auto colorschemes = doc["color_schemes"]; colorschemes)
    {
        usedKeys.emplace("color_schemes");

        for (auto i = colorschemes.begin(); i != colorschemes.end(); ++i)
        {
            auto const name = i->first.as<string>();
            auto const path = "color_schemes." + name;
            updateColorScheme(config.colorschemes[name], usedKeys, path, i->second);
        }
    }

    tryLoadValue(usedKeys, doc, "platform_plugin", config.platformPlugin, logger);
    if (config.platformPlugin == "auto")
        config.platformPlugin = ""; // Mapping "auto" to its internally equivalent "".

    string renderingBackendStr;
    if (tryLoadValue(usedKeys, doc, "renderer.backend", renderingBackendStr, logger))
    {
        renderingBackendStr = toUpper(renderingBackendStr);
        if (renderingBackendStr == "OPENGL"sv)
            config.renderingBackend = RenderingBackend::OpenGL;
        else if (renderingBackendStr == "SOFTWARE"sv)
            config.renderingBackend = RenderingBackend::Software;
        else if (renderingBackendStr != ""sv && renderingBackendStr != "DEFAULT"sv)
            errorLog()("Unknown renderer: {}.", renderingBackendStr);
    }

    tryLoadValue(
        usedKeys, doc, "renderer.tile_hashtable_slots", config.textureAtlasHashtableSlots.value, logger);
    tryLoadValue(usedKeys, doc, "renderer.tile_cache_count", config.textureAtlasTileCount.value, logger);
    tryLoadValue(usedKeys, doc, "renderer.tile_direct_mapping", config.textureAtlasDirectMapping, logger);

    if (doc["mock_font_locator"].IsSequence())
    {
        vector<text::font_description_and_source> registry;
        usedKeys.emplace("mock_font_locator");
        for (size_t i = 0; i < doc["mock_font_locator"].size(); ++i)
        {
            auto const node = doc["mock_font_locator"][i];
            auto const fontBasePath = fmt::format("mock_font_locator.{}", i);
            text::font_description_and_source fds;
            softLoadFont(usedKeys, fontBasePath, node, fds.description);
            fds.source = text::font_path { node["path"].as<string>() };
            usedKeys.emplace(fmt::format("{}.path", fontBasePath));
            registry.emplace_back(std::move(fds));
        }
        text::mock_font_locator::configure(std::move(registry));
    }

    tryLoadValue(usedKeys, doc, "read_buffer_size", config.ptyReadBufferSize, logger);
    if ((config.ptyReadBufferSize % 16) != 0)
    {
        // For improved performance ...
        logger()("read_buffer_size must be a multiple of 16.");
    }

    tryLoadValue(usedKeys, doc, "pty_buffer_size", config.ptyBufferObjectSize, logger);
    if (config.ptyBufferObjectSize < 1024 * 256)
    {
        // For improved performance ...
        logger()("pty_buffer_size too small. This cann severily degrade performance. Forcing 256 KB as "
                 "minimum acceptable setting.");
        config.ptyBufferObjectSize = 1024 * 256;
    }

    tryLoadValue(usedKeys, doc, "reflow_on_resize", config.reflowOnResize, logger);

    tryLoadValue(usedKeys, doc, "default_profile", config.defaultProfileName, logger);

    if (auto profiles = doc["profiles"])
    {
        auto const parentPath = "profiles"s;

        usedKeys.emplace("profiles");
        usedKeys.emplace(fmt::format("{}.{}", parentPath, config.defaultProfileName));
        auto const& defaultProfileNode = profiles[config.defaultProfileName];
        if (!defaultProfileNode)
        {
            logger()("default_profile \"{}\" not found in profiles list."
                     " Using the first available profile",
                     escape(config.defaultProfileName));

            if (profiles.begin() != profiles.end())
                config.defaultProfileName = profiles.begin()->first.as<std::string>();
            else
                throw std::runtime_error("No profile is defined in config, exiting contour.");
        }
        config.profiles[config.defaultProfileName] = loadTerminalProfile(usedKeys,
                                                                         profiles[config.defaultProfileName],
                                                                         parentPath,
                                                                         config.defaultProfileName,
                                                                         config.colorschemes);

        if (!config.defaultProfileName.empty() && config.profile(config.defaultProfileName) == nullptr)
        {
            errorLog()("default_profile \"{}\" not found in profiles list.",
                       escape(config.defaultProfileName));
        }
        auto dummy = logstore::category("dymmy", "empty logger", logstore::category::state::Disabled);

        for (auto i = profiles.begin(); i != profiles.end(); ++i)
        {
            auto const& name = i->first.as<string>();
            if (name == config.defaultProfileName)
                continue;
            auto const profile = i->second;
            usedKeys.emplace(fmt::format("{}.{}", parentPath, name));
            config.profiles[name] = config.profiles[config.defaultProfileName];
            updateTerminalProfile(
                config.profiles[name], usedKeys, profile, parentPath, name, config.colorschemes, configLog);
        }
    }

    if (auto mapping = doc["input_mapping"]; mapping)
    {
        usedKeys.emplace("input_mapping");
        if (mapping.IsSequence())
            for (size_t i = 0; i < mapping.size(); ++i)
            {
                auto prefix = fmt::format("{}.{}", "input_mapping", i);
                parseInputMapping(usedKeys, prefix, config, mapping[i]);
            }
    }

    checkForSuperfluousKeys(doc, usedKeys);
}

optional<std::string> readConfigFile(std::string const& filename)
{
    for (fs::path const& prefix: configHomes("contour"))
        if (auto text = readFile(prefix / filename); text.has_value())
            return text;

    return nullopt;
}

} // namespace contour::config
