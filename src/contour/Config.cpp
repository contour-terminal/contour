/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Config.h"
#include "contour_yaml.h"

#include <terminal/InputGenerator.h>
#include <terminal/Process.h>
#include <terminal/ControlCode.h>

#include <crispy/logstore.h>
#include <crispy/overloaded.h>
#include <crispy/stdfs.h>
#include <crispy/escape.h>
#include <crispy/utils.h>

#include <yaml-cpp/yaml.h>
#include <yaml-cpp/ostream_wrapper.h>

#include <QtGui/QOpenGLContext>

#include <array>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

auto constexpr MinimumFontSize = text::font_size{ 8.0 };

using namespace std;
using crispy::escape;
using crispy::unescape;
using crispy::toLower;
using crispy::toUpper;

using terminal::ImageSize;
using terminal::Width;
using terminal::Height;

using terminal::PageSize;
using terminal::LineCount;
using terminal::ColumnCount;


namespace contour::config {

namespace {
    auto const ConfigLog = logstore::Category("config", "Logs configuration file loading.");
}

using actions::Action;

// TODO:
// - [x] report missing keys
// - [ ] report superfluous keys (by keeping track of loaded keys, then iterate
//       through full document and report any key that has not been loaded but is available)
// - [ ] Do we want to report when no color schemes are defined? (at least warn about?)
// - [ ] Do we want to report when no input mappings are defined? (at least warn about?)

namespace // {{{ helper
{
    vector<FileSystem::path> getTermInfoDirs(optional<FileSystem::path> const& _appTerminfoDir)
    {
        auto locations = vector<FileSystem::path>();

        if (_appTerminfoDir.has_value())
            locations.emplace_back(_appTerminfoDir.value().string());

        locations.emplace_back(getenv("HOME") + "/.terminfo"s);

        if (auto const value = getenv("TERMINFO_DIRS"); value && *value)
            for (auto const dir: crispy::split(string_view(value), ':'))
                locations.push_back(FileSystem::path(string(dir)));

        locations.emplace_back("/usr/share/terminfo");

        return locations;
    }

    string getDefaultTERM(optional<FileSystem::path> const& _appTerminfoDir)
    {
#if defined(_WIN32)
        return "contour";
#else
        auto locations = getTermInfoDirs(_appTerminfoDir);
        auto const terms = vector<string>{
            "contour",
            "contour-latest",
            "xterm-256color",
            "xterm",
            "vt340",
            "vt220",
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

    optional<Permission> toPermission(string const& _value)
    {
        if (_value == "allow")
            return Permission::Allow;
        else if (_value == "deny")
            return Permission::Deny;
        else if (_value == "ask")
            return Permission::Ask;
        return nullopt;
    }

    void createFileIfNotExists(FileSystem::path const& _path)
    {
        if (!FileSystem::is_regular_file(_path))
            if (auto const ec = createDefaultConfig(_path); ec)
                throw runtime_error{fmt::format("Could not create directory {}. {}",
                                                _path.parent_path().string(),
                                                ec.message())};
    }

    using UsedKeys = set<string>;

    template <typename T>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      vector<string_view> const& _keys,
                      size_t _offset,
                      T& _store)
    {
        string parentKey;
        for (size_t i = 0; i < _offset; ++i)
        {
            if (i)
                parentKey += '.';
            parentKey += _keys.at(i);
        }

        if (_offset == _keys.size())
        {
            _store = _root.as<T>();
            return true;
        }

        auto const currentKey = string(_keys.at(_offset));

        auto const child = _root[currentKey];
        if (!child)
        {
            auto const defaultStr = crispy::escape(fmt::format("{}", _store));
            for (size_t i = _offset; i < _keys.size(); ++i)
            {
                parentKey += '.';
                parentKey += _keys[i];
            }
            LOGSTORE(ConfigLog)(
                "Missing key {}. Using default: {}.",
                parentKey, !defaultStr.empty() ? defaultStr : "\"\""s
            );
            return false;
        }

        _usedKeys.emplace(parentKey);

        return tryLoadValue(_usedKeys, child, _keys, _offset + 1, _store);
    }

    template <typename T, typename U>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      vector<string_view> const& _keys,
                      size_t _offset,
                      crispy::boxed<T, U>& _store)
    {
        return tryLoadValue(_usedKeys, _root, _keys, _offset, _store.value);
    }

    template <typename T>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      string const& _path,
                      T& _store)
    {
        auto const keys = crispy::split(_path, '.');
        _usedKeys.emplace(_path);
        return tryLoadValue(_usedKeys, _root, keys, 0, _store);
    }

    template <typename T, typename U>
    bool tryLoadValue(UsedKeys& _usedKeys,
                      YAML::Node const& _root,
                      string const& _path,
                      crispy::boxed<T, U>& _store)
    {
        return tryLoadValue(_usedKeys, _root, _path, _store.value);
    }

    template <typename T>
    bool tryLoadChild(UsedKeys& _usedKeys,
                      YAML::Node const& _doc,
                      string const& _parentPath,
                      string const& _key,
                      T& _store)
    {
        auto const path = fmt::format("{}.{}", _parentPath, _key);
        return tryLoadValue(_usedKeys, _doc, path, _store);
    }

    template <typename T, typename U>
    bool tryLoadChild(UsedKeys& _usedKeys,
                      YAML::Node const& _doc,
                      string const& _parentPath,
                      string const& _key,
                      crispy::boxed<T, U>& _store)
    {
        return tryLoadChild(_usedKeys, _doc, _parentPath, _key, _store.value);
    }

    void checkForSuperfluousKeys(YAML::Node _root,
                                 string const& _prefix,
                                 UsedKeys const& _usedKeys)
    {
        if (_root.IsMap())
        {
            for (auto const& mapItem: _root)
            {
                auto const name = mapItem.first.as<string>();
                auto const child = mapItem.second;
                auto const prefix = _prefix.empty() ? name : fmt::format("{}.{}", _prefix, name);
                checkForSuperfluousKeys(child, prefix, _usedKeys);
                if (_usedKeys.count(prefix))
                    continue;
                if (crispy::startsWith(string_view(prefix), "x-"sv))
                    continue;
                errorlog()(
                    "Superfluous config key found: {}",
                    escape(prefix)
                );
            }
        }
        else if (_root.IsSequence())
        {
            for (size_t i = 0; i < _root.size() && i < 8; ++i)
            {
                checkForSuperfluousKeys(
                    _root[i],
                    fmt::format("{}.{}", _prefix, i),
                    _usedKeys
                );
            }
        }
#if 0
        else if (_root.IsScalar())
        {
        }
        else if (_root.IsNull())
        {
            ; // no-op
        }
#endif
    }

    void checkForSuperfluousKeys(YAML::Node const& _root, UsedKeys const& _usedKeys)
    {
        checkForSuperfluousKeys(_root, "", _usedKeys);
    }

    optional<std::string> readFile(FileSystem::path const& _path)
    {
        if (!FileSystem::exists(_path))
            return nullopt;

        auto ifs = ifstream(_path.string());
        if (!ifs.good())
            return nullopt;

        auto const size = FileSystem::file_size(_path);
        auto text = string{};
        text.resize(size);
        ifs.read(text.data(), static_cast<std::streamsize>(size));
        return {text};
    }

    terminal::CellRGBColor parseCellColor(std::string const& _text)
    {
        auto const text = toUpper(_text);
        if (text == "CELLBACKGROUND"sv)
            return terminal::CellBackgroundColor{};
        if (text == "CELLFOREGROUND"sv)
            return terminal::CellForegroundColor{};
        return terminal::RGBColor(_text);
    }
}
// }}}

FileSystem::path configHome(string const& _programName)
{
#if defined(__unix__) || defined(__APPLE__)
	if (auto const *value = getenv("XDG_CONFIG_HOME"); value && *value)
		return FileSystem::path{value} / _programName;
	else if (auto const *value = getenv("HOME"); value && *value)
		return FileSystem::path{value} / ".config" / _programName;
#endif

#if defined(_WIN32)
	DWORD size = GetEnvironmentVariable("LOCALAPPDATA", nullptr, 0);
	if (size)
	{
		std::vector<char> buf;
		buf.resize(size);
		GetEnvironmentVariable("LOCALAPPDATA", &buf[0], size);
		return FileSystem::path{&buf[0]} / _programName;
	}
#endif

	throw runtime_error{"Could not find config home folder."};
}

std::vector<FileSystem::path> configHomes(string const& _programName)
{
    std::vector<FileSystem::path> paths;

#if defined(CONTOUR_PROJECT_SOURCE_DIR) && !defined(NDEBUG)
    paths.emplace_back(FileSystem::path(CONTOUR_PROJECT_SOURCE_DIR) / "src" / "terminal_view" / "shaders");
#endif

    paths.emplace_back(configHome(_programName));

#if defined(__unix__) || defined(__APPLE__)
    paths.emplace_back(FileSystem::path("/etc") / _programName);
#endif

    return paths;
}

FileSystem::path configHome()
{
    return configHome("contour");
}

std::string createDefaultConfig()
{
    ostringstream out;
    out.write(
        (char const*) contour::default_config_yaml.data(),
        contour::default_config_yaml.size()
    );

    return out.str();
}

error_code createDefaultConfig(FileSystem::path const& _path)
{
    FileSystemError ec;
    FileSystem::create_directories(_path.parent_path(), ec);
    if (ec)
        return ec;

    ofstream{_path.string(), ios::binary | ios::trunc} << createDefaultConfig();

    return error_code{};
}

template <typename T>
inline auto mapAction(std::string_view _name)
{
	return pair{_name, Action{T{}}};
}

optional<terminal::Key> parseKey(string const& _name)
{
    using terminal::Key;
	auto static constexpr mappings = array{
		pair{ "F1"sv, Key::F1 },
		pair{ "F2"sv, Key::F2 },
		pair{ "F3"sv, Key::F3 },
		pair{ "F4"sv, Key::F4 },
		pair{ "F5"sv, Key::F5 },
		pair{ "F6"sv, Key::F6 },
		pair{ "F7"sv, Key::F7 },
		pair{ "F8"sv, Key::F8 },
		pair{ "F9"sv, Key::F9 },
		pair{ "F10"sv, Key::F10 },
		pair{ "F11"sv, Key::F11 },
		pair{ "F12"sv, Key::F12 },
		pair{ "DownArrow"sv, Key::DownArrow },
		pair{ "LeftArrow"sv, Key::LeftArrow },
		pair{ "RightArrow"sv, Key::RightArrow },
		pair{ "UpArrow"sv, Key::UpArrow },
		pair{ "Insert"sv, Key::Insert },
		pair{ "Delete"sv, Key::Delete },
		pair{ "Home"sv, Key::Home },
		pair{ "End"sv, Key::End },
		pair{ "PageUp"sv, Key::PageUp },
		pair{ "PageDown"sv, Key::PageDown },
		pair{ "Numpad_NumLock"sv, Key::Numpad_NumLock },
		pair{ "Numpad_Divide"sv, Key::Numpad_Divide },
		pair{ "Numpad_Multiply"sv, Key::Numpad_Multiply },
		pair{ "Numpad_Subtract"sv, Key::Numpad_Subtract },
		pair{ "Numpad_CapsLock"sv, Key::Numpad_CapsLock },
		pair{ "Numpad_Add"sv, Key::Numpad_Add },
		pair{ "Numpad_Decimal"sv, Key::Numpad_Decimal },
		pair{ "Numpad_Enter"sv, Key::Numpad_Enter },
		pair{ "Numpad_Equal"sv, Key::Numpad_Equal },
		pair{ "Numpad_0"sv, Key::Numpad_0 },
		pair{ "Numpad_1"sv, Key::Numpad_1 },
		pair{ "Numpad_2"sv, Key::Numpad_2 },
		pair{ "Numpad_3"sv, Key::Numpad_3 },
		pair{ "Numpad_4"sv, Key::Numpad_4 },
		pair{ "Numpad_5"sv, Key::Numpad_5 },
		pair{ "Numpad_6"sv, Key::Numpad_6 },
		pair{ "Numpad_7"sv, Key::Numpad_7 },
		pair{ "Numpad_8"sv, Key::Numpad_8 },
		pair{ "Numpad_9"sv, Key::Numpad_9 }
    };

    auto const name = toLower(_name);

    for (auto const& mapping: mappings)
        if (name == toLower(mapping.first))
            return mapping.second;

    return nullopt;
}

optional<variant<terminal::Key, char32_t>> parseKeyOrChar(string const& _name)
{
    using namespace terminal::ControlCode;

    if (auto const key = parseKey(_name); key.has_value())
        return key.value();

    auto const text = QString::fromUtf8(_name.c_str()).toUcs4();
    if (text.size() == 1)
        return static_cast<char32_t>(toupper(text[0]));

    auto constexpr namedChars = array{
        pair{"ENTER"sv, (char) C0::CR},
        pair{"BACKSPACE"sv, (char) C0::BS },
        pair{"TAB"sv, (char) C0::HT },
        pair{"ESCAPE"sv, (char) C0::ESC },

        pair{"LESS"sv, '<'},
        pair{"GREATER"sv, '>'},
        pair{"PLUS"sv, '+'},

        pair{"APOSTROPHE"sv, '\''},
        pair{"ADD"sv, '+'},
        pair{"BACKSLASH"sv, 'x'},
        pair{"COMMA"sv, ','},
        pair{"DECIMAL"sv, '.'},
        pair{"DIVIDE"sv, '/'},
        pair{"EQUAL"sv, '='},
        pair{"LEFT_BRACKET"sv, '['},
        pair{"MINUS"sv, '-'},
        pair{"MULTIPLY"sv, '*'},
        pair{"PERIOD"sv, '.'},
        pair{"RIGHT_BRACKET"sv, ']'},
        pair{"SEMICOLON"sv, ';'},
        pair{"SLASH"sv, '/'},
        pair{"SUBTRACT"sv, '-'},
        pair{"SPACE"sv, ' '}
    };

    auto const name = toUpper(_name);
    for (auto const& mapping: namedChars)
        if (name == mapping.first)
            return static_cast<char32_t>(mapping.second);

    return nullopt;
}

optional<terminal::Modifier::Key> parseModifierKey(string const& _key)
{
    using terminal::Modifier;
    auto const key = toUpper(_key);
    if (key == "ALT")
        return Modifier::Key::Alt;
    if (key == "CONTROL")
        return Modifier::Key::Control;
    if (key == "SHIFT")
        return Modifier::Key::Shift;
    if (key == "META")
        return Modifier::Key::Meta;
    return nullopt;
}

optional<terminal::MatchModes> parseMatchModes(UsedKeys& _usedKeys,
                                               string const& _prefix,
                                               YAML::Node const& _node)
{
    using terminal::MatchModes;
    if (!_node)
        return terminal::MatchModes{};
    _usedKeys.emplace(_prefix);
    if (!_node.IsScalar())
        return nullopt;

    auto matchModes = MatchModes{};

    auto const modeStr = _node.as<string>();
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
        else if (upperArg == "SELECT")
            flag = MatchModes::Select;
        else
        {
            errorlog()("Unknown input_mapping mode: {}", arg);
            continue;
        }

        if (negate)
            matchModes.disable(flag);
        else
            matchModes.enable(flag);
    }

    return matchModes;
}

optional<terminal::Modifier> parseModifier(UsedKeys& _usedKeys,
                                           string const& _prefix,
                                           YAML::Node const& _node)
{
    using terminal::Modifier;
    if (!_node)
        return nullopt;
    _usedKeys.emplace(_prefix);
    if (_node.IsScalar())
        return parseModifierKey(_node.as<string>());
    if (!_node.IsSequence())
        return nullopt;

    terminal::Modifier mods;
    for (size_t i = 0; i < _node.size(); ++i)
    {
        if (!_node[i].IsScalar())
            return nullopt;

        auto const mod = parseModifierKey(_node[i].as<string>());
        if (!mod)
            return nullopt;

        mods |= *mod;
    }
    return mods;
}

namespace // {{{ helper
{

template <typename Input>
void appendOrCreateBinding(
    vector<terminal::InputBinding<Input, ActionList>>& _bindings,
    terminal::MatchModes _modes,
    terminal::Modifier _modifier,
    Input _input,
    Action _action)
{
    for (auto& binding: _bindings)
    {
        if (match(binding, _modes, _modifier, _input))
        {
            binding.binding.emplace_back(move(_action));
            return;
        }
    }

    _bindings.emplace_back(terminal::InputBinding<Input, ActionList>{
        _modes, _modifier, _input, ActionList{move(_action)}
    });
}

bool tryAddKey(InputMappings& _inputMappings,
               terminal::MatchModes _modes,
               terminal::Modifier _modifier,
               YAML::Node const& _node,
               Action _action)
{
    if (!_node)
        return false;

    if (!_node.IsScalar())
        return false;

    auto const input = parseKeyOrChar(_node.as<string>());
    if (!input.has_value())
        return false;

    if (holds_alternative<terminal::Key>(*input))
    {
        appendOrCreateBinding(_inputMappings.keyMappings, _modes, _modifier,
                              get<terminal::Key>(*input), move(_action));
    }
    else if (holds_alternative<char32_t>(*input))
    {
        appendOrCreateBinding(_inputMappings.charMappings, _modes, _modifier,
                              get<char32_t>(*input), move(_action));
    }
    else
        assert(false && "The impossible happened.");

    return true;
}

optional<terminal::MouseButton> parseMouseButton(YAML::Node const& _node)
{
    if (!_node)
        return nullopt;

    if (!_node.IsScalar())
        return nullopt;

    auto constexpr static mappings = array{
        pair{"WHEELUP"sv, terminal::MouseButton::WheelUp},
        pair{"WHEELDOWN"sv, terminal::MouseButton::WheelDown},
        pair{"LEFT"sv, terminal::MouseButton::Left},
        pair{"MIDDLE"sv, terminal::MouseButton::Middle},
        pair{"RIGHT"sv, terminal::MouseButton::Right},
    };
    auto const name = toUpper(_node.as<string>());
    for (auto const& mapping: mappings)
        if (name == mapping.first)
            return mapping.second;
    return nullopt;
}

bool tryAddMouse(vector<MouseInputMapping>& _bindings,
                 terminal::MatchModes _modes,
                 terminal::Modifier _modifier,
                 YAML::Node const& _node,
                 Action _action)
{
    auto mouseButton = parseMouseButton(_node);
    if (!mouseButton)
        return false;

    appendOrCreateBinding(_bindings, _modes, _modifier, *mouseButton, move(_action));
    return true;
}

optional<Action> parseAction(UsedKeys& _usedKeys,
                             string const& _prefix,
                             Config& _config,
                             YAML::Node const& _parent)
{
    _usedKeys.emplace(_prefix + ".action");

    auto actionName = _parent["action"].as<string>();
    _usedKeys.emplace(_prefix + ".action." + actionName);
    auto actionOpt = actions::fromString(actionName);
    if (!actionOpt)
    {
        cerr << "Unknown action: '" << _parent["action"].as<string>() << '\'' << endl;
        return nullopt;
    }

    auto action = actionOpt.value();

    if (holds_alternative<actions::ChangeProfile>(action))
    {
        if (auto name = _parent["name"]; name.IsScalar())
        {
            _usedKeys.emplace(_prefix + ".name");
            return actions::ChangeProfile{name.as<string>()};
        }
        else
            return nullopt;
    }

    if (holds_alternative<actions::NewTerminal>(action))
    {
        if (auto profile = _parent["profile"]; profile && profile.IsScalar())
        {
            _usedKeys.emplace(_prefix + ".profile");
            return actions::NewTerminal{profile.as<string>()};
        }
        else
            return action;
    }

    if (holds_alternative<actions::ReloadConfig>(action))
    {
        _usedKeys.emplace(_prefix + ".profile");
        if (auto profileName = _parent["profile"]; profileName.IsScalar())
        {
            _usedKeys.emplace(_prefix + ".profile");
            return actions::ReloadConfig{profileName.as<string>()};
        }
        else
            return action;
    }

    if (holds_alternative<actions::SendChars>(action))
    {
        if (auto chars = _parent["chars"]; chars.IsScalar())
        {
            _usedKeys.emplace(_prefix + ".chars");
            return actions::SendChars{unescape(chars.as<string>())};
        }
        else
            return nullopt;
    }

    if (holds_alternative<actions::WriteScreen>(action))
    {
        if (auto chars = _parent["chars"]; chars.IsScalar())
        {
            _usedKeys.emplace(_prefix + ".chars");
            return actions::WriteScreen{unescape(chars.as<string>())};
        }
        else
            return nullopt;
    }

    return action;
}

void parseInputMapping(UsedKeys& _usedKeys, string const& _prefix,
                       Config& _config, YAML::Node const& _mapping)
{
	using namespace terminal;

    auto const action = parseAction(_usedKeys, _prefix, _config, _mapping);
	auto const mods = parseModifier(_usedKeys, _prefix + ".mods", _mapping["mods"]);
    auto const mode = parseMatchModes(_usedKeys, _prefix + ".mode", _mapping["mode"]);
    if (action && mods && mode)
    {
        if (tryAddKey(_config.inputMappings, *mode, *mods, _mapping["key"], *action))
        {
            _usedKeys.emplace(_prefix + ".key");
        }
        else if (tryAddMouse(_config.inputMappings.mouseMappings, *mode, *mods, _mapping["mouse"], *action))
        {
            _usedKeys.emplace(_prefix + ".mouse");
        }
        else
        {
            // TODO: log error: invalid key mapping at: _mapping.sourceLocation()
            LOGSTORE(ConfigLog)("Could not add some input mapping.");
        }
    }
}

}
// }}}

std::string defaultConfigFilePath()
{
    return (configHome() / "contour.yml").string();
}

Config loadConfig()
{
    return loadConfigFromFile(defaultConfigFilePath());
}

Config loadConfigFromFile(FileSystem::path const& _fileName)
{
    Config config{};
    loadConfigFromFile(config, _fileName);
    return config;
}

terminal::ColorPalette loadColorScheme(
        UsedKeys& _usedKeys,
        string const& _path,
        YAML::Node const& _node)
{
    auto colors = terminal::ColorPalette{};
    if (!_node)
        return colors;

    _usedKeys.emplace(_path);
    using terminal::RGBColor;
    if (auto def = _node["default"]; def)
    {
        _usedKeys.emplace(_path + ".default");
        if (auto fg = def["foreground"]; fg)
        {
            _usedKeys.emplace(_path + ".default.foreground");
            colors.defaultForeground = fg.as<string>();
        }
        if (auto bg = def["background"]; bg)
        {
            _usedKeys.emplace(_path + ".default.background");
            colors.defaultBackground = bg.as<string>();
        }
    }

    if (auto def = _node["selection"]; def && def.IsMap())
    {
        _usedKeys.emplace(_path + ".selection");
        if (auto fg = def["foreground"]; fg && fg.IsScalar())
        {
            _usedKeys.emplace(_path + ".selection.foreground");
            colors.selectionForeground.emplace(fg.as<string>());
        }
        else
            colors.selectionForeground.reset();

        if (auto bg = def["background"]; bg && bg.IsScalar())
        {
            _usedKeys.emplace(_path + ".selection.background");
            colors.selectionBackground.emplace(bg.as<string>());
        }
        else
            colors.selectionBackground.reset();
    }

    if (auto cursor = _node["cursor"]; cursor)
    {
        _usedKeys.emplace(_path + ".cursor");
        if (cursor.IsMap())
        {
            if (auto color = cursor["default"]; color.IsScalar())
            {
                _usedKeys.emplace(_path + ".cursor.default");
                colors.cursor.color = parseCellColor(color.as<string>());
            }
            if (auto color = cursor["text"]; color.IsScalar())
            {
                _usedKeys.emplace(_path + ".cursor.text");
                colors.cursor.textOverrideColor = parseCellColor(color.as<string>());
            }
        }
        else if (cursor.IsScalar())
        {
            errorlog()("Deprecated cursor config colorscheme entry. Please update your colorscheme entry for cursor.");
            colors.cursor.color = RGBColor(cursor.as<string>());
        }
        else
            errorlog()("Invalid cursor config colorscheme entry.");
    }

    if (auto hyperlink = _node["hyperlink_decoration"]; hyperlink)
    {
        _usedKeys.emplace(_path + ".hyperlink_decoration");
        if (auto color = hyperlink["normal"]; color && color.IsScalar() && !color.as<string>().empty())
        {
            _usedKeys.emplace(_path + ".hyperlink_decoration.normal");
            colors.hyperlinkDecoration.normal = color.as<string>();
        }

        if (auto color = hyperlink["hover"]; color && color.IsScalar() && !color.as<string>().empty())
        {
            _usedKeys.emplace(_path + ".hyperlink_decoration.hover");
            colors.hyperlinkDecoration.hover = color.as<string>();
        }
    }

    auto const loadColorMap = [&](YAML::Node const& _parent, string const& _key, size_t _offset) {
        auto node = _parent[_key];
        if (!node)
            return;

        auto const colorKeyPath = fmt::format("{}.{}", _path, _key);
        _usedKeys.emplace(colorKeyPath);
        if (node.IsMap())
        {
            auto const assignColor = [&](size_t _index, string const& _name) {
                if (auto nodeValue = node[_name]; nodeValue)
                {
                    _usedKeys.emplace(fmt::format("{}.{}", colorKeyPath, _name));
                    if (auto const value = nodeValue.as<string>(); !value.empty())
                    {
                        if (value[0] == '#')
                            colors.palette[_offset + _index] = value;
                        else if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
                            colors.palette[_offset + _index] = nodeValue.as<unsigned long>();
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
        }
        else if (node.IsSequence())
        {
            for (size_t i = 0; i < node.size() && i < 8; ++i)
                if (node[i].IsScalar())
                    colors.palette[i] = node[i].as<long>();
                else
                    colors.palette[i] = node[i].as<string>();
        }
    };

    loadColorMap(_node, "normal", 0);
    loadColorMap(_node, "bright", 8);
    // TODO: color palette from 16..255
    // TODO: dim _node (maybe put them into the palette at 256..(256+8)?)

    return colors;
}

void softLoadFont(terminal::renderer::TextShapingEngine _textShapingEngine,
                  UsedKeys& _usedKeys,
                  string_view _basePath,
                  YAML::Node const& _node,
                  string_view _key,
                  text::font_description& _store)
{
    auto node = _node[string(_key)];
    if (!node)
        return;

    if (node.IsScalar())
    {
        _store.familyName = node.as<string>();
        _usedKeys.emplace(fmt::format("{}.{}", _basePath, _key));
    }
    else if (node.IsMap())
    {
        _usedKeys.emplace(fmt::format("{}.{}", _basePath, _key));

        if (node["family"].IsScalar())
        {
            _usedKeys.emplace(fmt::format("{}.{}.{}", _basePath, _key, "family"));
            _store.familyName = node["family"].as<string>();
        }

        if (node["slant"].IsScalar())
        {
            _usedKeys.emplace(fmt::format("{}.{}.{}", _basePath, _key, "slant"));
            if (auto const p = text::make_font_slant(node["slant"].as<string>()))
                _store.slant = p.value();
        }

        if (node["weight"].IsScalar())
        {
            _usedKeys.emplace(fmt::format("{}.{}.{}", _basePath, _key, "weight"));
            if (auto const p = text::make_font_weight(node["weight"].as<string>()))
                _store.weight = p.value();
        }

        if (node["features"].IsSequence())
        {
            _usedKeys.emplace(fmt::format("{}.{}.{}", _basePath, _key, "features"));
            YAML::Node featuresNode = node["features"];
            for (int i = 0; i < featuresNode.size(); ++i)
            {
                YAML::Node const featureNode = featuresNode[i];
                if (!featureNode.IsScalar() || featureNode.as<string>().size() != 4)
                {
                    errorlog()("Invalid font feature \"{}\".", featureNode.as<string>());
                    continue;
                }
                auto const tag = featureNode.as<string>();
                _store.features.emplace_back(tag[0], tag[1], tag[2], tag[3]);
            }
            using terminal::renderer::TextShapingEngine;
            switch (_textShapingEngine)
            {
                case TextShapingEngine::OpenShaper:
                    break;
                case TextShapingEngine::CoreText:
                case TextShapingEngine::DWrite:
                    // TODO: Implement font feature settings handling for these engines.
                    errorlog()("The configured text shaping engine {} does not yet support font feature settings. Ignoring.", _textShapingEngine);
            }
        }
    }
}

template <typename T>
bool sanitizeRange(std::reference_wrapper<T> _value, T _min, T _max)
{
    if (_min <= _value.get() && _value.get() <= _max)
        return true;

    _value.get() = std::clamp(_value.get(), _min, _max);
    return false;
}

optional<terminal::VTType> stringToVTType(std::string const& _value)
{
    using Type = terminal::VTType;
    auto constexpr static mappings = array<tuple<string_view, terminal::VTType>, 10>{
        tuple{"VT100"sv, Type::VT100},
        tuple{"VT220"sv, Type::VT220},
        tuple{"VT240"sv, Type::VT240},
        tuple{"VT330"sv, Type::VT330},
        tuple{"VT340"sv, Type::VT340},
        tuple{"VT320"sv, Type::VT320},
        tuple{"VT420"sv, Type::VT420},
        tuple{"VT510"sv, Type::VT510},
        tuple{"VT520"sv, Type::VT520},
        tuple{"VT525"sv, Type::VT525}
    };
    for (auto const& mapping: mappings)
        if (get<0>(mapping) == _value)
            return get<1>(mapping);
    return nullopt;
}

TerminalProfile loadTerminalProfile(UsedKeys& _usedKeys,
                                    YAML::Node const& _doc,
                                    std::string const& _name,
                                    unordered_map<string, terminal::ColorPalette> const& _colorschemes)
{
    auto profile = TerminalProfile{};

    if (auto colors = _doc["profiles"][_name]["colors"]; colors)
    {
        auto const path = fmt::format("{}.{}.{}", "profiles", _name, "colors");
        if (colors.IsMap())
            profile.colors = loadColorScheme(_usedKeys, path, colors);
        else if (auto i = _colorschemes.find(colors.as<string>()); i != _colorschemes.end())
        {
            _usedKeys.emplace(path);
            profile.colors = i->second;
        }
        else if (colors.IsScalar())
        {
            bool found = false;
            for (FileSystem::path const& prefix: configHomes("contour"))
            {
                auto const filePath = prefix / "colorschemes" / (colors.as<string>() + ".yml");
                auto fileContents = readFile(filePath);
                if (!fileContents)
                    continue;
                YAML::Node subDocument = YAML::Load(fileContents.value());
                UsedKeys usedColorKeys;
                profile.colors = loadColorScheme(usedColorKeys, "", subDocument);
                // TODO: Check usedColorKeys for validity.
                LOGSTORE(ConfigLog)("Loaded colors from {}.", filePath.string());
                found = true;
                break;
            }
            if (!found)
                errorlog()("Could not open colorscheme file for \"{}\".", colors.as<string>());
        }
        else
            errorlog()("scheme '{}' not found.", colors.as<string>());
    }
    else
        errorlog()("No colors section in profile {} found.", _name);

    string const basePath = fmt::format("profiles.{}", _name);
    tryLoadChild(_usedKeys, _doc, basePath, "shell", profile.shell.program);
    if (profile.shell.program.empty())
    {
        if (!profile.shell.arguments.empty())
            errorlog()("No shell defined but arguments. Ignoring arguments.");

        auto loginShell = terminal::Process::loginShell();
        profile.shell.program = loginShell.front();
        loginShell.erase(loginShell.begin());
        profile.shell.arguments = loginShell;
    }
    tryLoadChild(_usedKeys, _doc, basePath, "maximized", profile.maximized);
    tryLoadChild(_usedKeys, _doc, basePath, "fullscreen", profile.fullscreen);
    tryLoadChild(_usedKeys, _doc, basePath, "refresh_rate", profile.refreshRate);

    tryLoadChild(_usedKeys, _doc, basePath, "wm_class", profile.wmClass);

    if (auto args = _doc["profiles"][_name]["arguments"]; args && args.IsSequence())
    {
        _usedKeys.emplace(fmt::format("{}.arguments", basePath));
        for (auto const& argNode : args)
            profile.shell.arguments.emplace_back(argNode.as<string>());
    }

    string strValue = FileSystem::current_path().generic_string();
    tryLoadChild(_usedKeys, _doc, basePath, "initial_working_directory", strValue);
    if (strValue.empty())
        profile.shell.workingDirectory = FileSystem::current_path();
    else if (strValue[0] == '~')
    {
        bool const delim = strValue.size() >= 2 && (strValue[1] == '/' || strValue[1] == '\\');
        auto const subPath = FileSystem::path(strValue.substr(delim ? 2 : 1));
        profile.shell.workingDirectory = terminal::Process::homeDirectory() / subPath;
    }
    else
        profile.shell.workingDirectory  = FileSystem::path(strValue);

    profile.shell.env["TERMINAL_NAME"] = "contour";
    profile.shell.env["TERMINAL_VERSION_TRIPLE"] = fmt::format("{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH);
    profile.shell.env["TERMINAL_VERSION_STRING"] = CONTOUR_VERSION_STRING;

    std::optional<FileSystem::path> appTerminfoDir;
    #if defined(__APPLE__)
    {
        char buf[1024];
        uint32_t len = sizeof(buf);
        if (_NSGetExecutablePath(buf, &len) == 0)
        {
            auto p = FileSystem::path(buf).parent_path().parent_path() / "Resources" / "terminfo";
            if (FileSystem::is_directory(p))
            {
                appTerminfoDir = p;
                profile.shell.env["TERMINFO_DIRS"] = p.string();
            }
        }
    }
    #endif

    if (auto env = _doc["profiles"][_name]["environment"]; env)
    {
        auto const envpath = basePath + ".environment";
        _usedKeys.emplace(envpath);
        for (auto i = env.begin(); i != env.end(); ++i)
        {
            auto const name = i->first.as<string>();
            auto const value = i->second.as<string>();
            _usedKeys.emplace(fmt::format("{}.{}", envpath, name));
            profile.shell.env[name] = value;
        }
    }

    // force some default env
    if (profile.shell.env.find("TERM") == profile.shell.env.end())
    {
        profile.shell.env["TERM"] = getDefaultTERM(appTerminfoDir);
        LOGSTORE(ConfigLog)("Defaulting TERM to {}.", profile.shell.env["TERM"]);
    }

    if (profile.shell.env.find("COLORTERM") == profile.shell.env.end())
        profile.shell.env["COLORTERM"] = "truecolor";

    strValue = fmt::format("{}", profile.terminalId);
    tryLoadChild(_usedKeys, _doc, basePath, "terminal_id", strValue);
    if (auto const idOpt = stringToVTType(strValue))
        profile.terminalId = idOpt.value();
    else
        errorlog()("Invalid Terminal ID \"{}\", specified", strValue);

    tryLoadChild(_usedKeys, _doc, basePath, "terminal_size.columns", profile.terminalSize.columns);
    tryLoadChild(_usedKeys, _doc, basePath, "terminal_size.lines", profile.terminalSize.lines);
    {
        auto constexpr MinimalTerminalSize = PageSize{LineCount(3), ColumnCount(3)};
        auto constexpr MaximumTerminalSize = PageSize{LineCount(200), ColumnCount(300)};

        if (!sanitizeRange(ref(profile.terminalSize.columns.value), *MinimalTerminalSize.columns, *MaximumTerminalSize.columns))
            errorlog()(
                "Terminal width {} out of bounds. Should be between {} and {}.",
                profile.terminalSize.columns, MinimalTerminalSize.columns, MaximumTerminalSize.columns
            );

        if (!sanitizeRange(ref(profile.terminalSize.lines), MinimalTerminalSize.lines, MaximumTerminalSize.lines))
            errorlog()(
                "Terminal height {} out of bounds. Should be between {} and {}.",
                profile.terminalSize.lines, MinimalTerminalSize.lines, MaximumTerminalSize.lines
            );
    }

    strValue = "ask";
    if (tryLoadChild(_usedKeys, _doc, basePath, "permissions.capture_buffer", strValue))
    {
        if (auto x = toPermission(strValue))
            profile.permissions.captureBuffer = x.value();
    }

    strValue = "ask";
    if (tryLoadChild(_usedKeys, _doc, basePath, "permissions.change_font", strValue))
    {
        if (auto x = toPermission(strValue))
            profile.permissions.changeFont = x.value();
    }

    if (tryLoadChild(_usedKeys, _doc, basePath, "font.size", profile.fonts.size.pt))
    {
        if (profile.fonts.size < MinimumFontSize)
        {
            errorlog()("Invalid font size {} set in config file. Minimum value is {}.",
                       profile.fonts.size, MinimumFontSize);
            profile.fonts.size = MinimumFontSize;
        }
    }

    tryLoadChild(_usedKeys, _doc, basePath, "font.builtin_box_drawing", profile.fonts.builtinBoxDrawing);
    tryLoadChild(_usedKeys, _doc, basePath, "font.dpi_scale", profile.fonts.dpiScale);

    auto constexpr NativeTextShapingEngine =
#if defined(_WIN32)
        terminal::renderer::TextShapingEngine::DWrite;
#elif defined(__APPLE__)
        terminal::renderer::TextShapingEngine::CoreText;
#else
        terminal::renderer::TextShapingEngine::OpenShaper;
#endif

    auto constexpr NativeFontLocator =
#if defined(_WIN32)
        terminal::renderer::FontLocatorEngine::DWrite;
#elif defined(__APPLE__)
        terminal::renderer::FontLocatorEngine::CoreText;
#else
        terminal::renderer::FontLocatorEngine::FontConfig;
#endif

    strValue = fmt::format("{}", profile.fonts.textShapingEngine);
    if (tryLoadChild(_usedKeys, _doc, basePath, "font.text_shaping.engine", strValue))
    {
        auto const lwrValue = toLower(strValue);
        if (lwrValue == "dwrite" || lwrValue == "directwrite")
            profile.fonts.textShapingEngine = terminal::renderer::TextShapingEngine::DWrite;
        else if (lwrValue == "core" || lwrValue == "coretext")
            profile.fonts.textShapingEngine = terminal::renderer::TextShapingEngine::CoreText;
        else if (lwrValue == "open" || lwrValue == "openshaper")
            profile.fonts.textShapingEngine = terminal::renderer::TextShapingEngine::OpenShaper;
        else if (lwrValue == "native")
            profile.fonts.textShapingEngine = NativeTextShapingEngine;
        else
            LOGSTORE(ConfigLog)("Invalid value for configuration key {}.font.text_shaping.engine: {}",
                    basePath, strValue);
    }

    profile.fonts.fontLocator = NativeFontLocator;
    strValue = fmt::format("{}", profile.fonts.fontLocator);
    if (tryLoadChild(_usedKeys, _doc, basePath, "font.locator", strValue))
    {
        auto const lwrValue = toLower(strValue);
        if (lwrValue == "fontconfig")
            profile.fonts.fontLocator = terminal::renderer::FontLocatorEngine::FontConfig;
        else if (lwrValue == "coretext")
            profile.fonts.fontLocator = terminal::renderer::FontLocatorEngine::CoreText;
        else if (lwrValue == "dwrite" || lwrValue == "directwrite")
            profile.fonts.fontLocator = terminal::renderer::FontLocatorEngine::DWrite;
        else if (lwrValue == "native")
            profile.fonts.fontLocator = NativeFontLocator;
        else
            LOGSTORE(ConfigLog)("Invalid value for configuration key {}.font.locator: {}",
                                basePath, strValue);
    }

    bool strictSpacing = false;
    tryLoadChild(_usedKeys, _doc, basePath, "font.strict_spacing", strictSpacing);

    auto const fontBasePath = fmt::format("profiles.{}.font", _name);

    profile.fonts.regular.familyName = "regular";
    profile.fonts.regular.spacing = text::font_spacing::mono;
    profile.fonts.regular.strict_spacing = strictSpacing;
    softLoadFont(profile.fonts.textShapingEngine, _usedKeys, fontBasePath, _doc["profiles"][_name]["font"], "regular", profile.fonts.regular);

    profile.fonts.bold = profile.fonts.regular;
    profile.fonts.bold.weight = text::font_weight::bold;
    softLoadFont(profile.fonts.textShapingEngine, _usedKeys, fontBasePath, _doc["profiles"][_name]["font"], "bold", profile.fonts.bold);

    profile.fonts.italic = profile.fonts.regular;
    profile.fonts.italic.slant = text::font_slant::italic;
    softLoadFont(profile.fonts.textShapingEngine, _usedKeys, fontBasePath, _doc["profiles"][_name]["font"], "italic", profile.fonts.italic);

    profile.fonts.boldItalic = profile.fonts.regular;
    profile.fonts.boldItalic.weight = text::font_weight::bold;
    profile.fonts.boldItalic.slant = text::font_slant::italic;
    softLoadFont(profile.fonts.textShapingEngine, _usedKeys, fontBasePath, _doc["profiles"][_name]["font"], "bold_italic", profile.fonts.boldItalic);

    profile.fonts.emoji.familyName = "emoji";
    profile.fonts.emoji.spacing = text::font_spacing::mono;
    softLoadFont(profile.fonts.textShapingEngine, _usedKeys, fontBasePath, _doc["profiles"][_name]["font"], "emoji", profile.fonts.emoji);

#if defined(_WIN32)
    // Windows does not understand font family "emoji", but fontconfig does. Rewrite user-input here.
    if (profile.fonts.emoji.familyName == "emoji")
        profile.fonts.emoji.familyName = "Segoe UI Emoji";
#endif

    strValue = "gray";
    string renderModeStr;
    tryLoadChild(_usedKeys, _doc, basePath, "font.render_mode", strValue);
    auto const static renderModeMap = array{
        pair{"lcd"sv, text::render_mode::lcd},
        pair{"light"sv, text::render_mode::light},
        pair{"gray"sv, text::render_mode::gray},
        pair{""sv, text::render_mode::gray},
        pair{"monochrome"sv, text::render_mode::bitmap},
    };

    auto const i = crispy::find_if(renderModeMap, [&](auto m) { return m.first == renderModeStr; });
    if (i != renderModeMap.end())
        profile.fonts.renderMode = i->second;
    else
        errorlog()("Invalid render_mode \"{}\" in configuration.", renderModeStr);

    auto intValue = profile.maxHistoryLineCount.value_or(std::numeric_limits<LineCount>::max());
    tryLoadChild(_usedKeys, _doc, basePath, "history.limit", intValue);
    if (unbox<int>(intValue) < 0 || intValue == std::numeric_limits<LineCount>::max())
        profile.maxHistoryLineCount = nullopt;
    else
        profile.maxHistoryLineCount = intValue;

    strValue = fmt::format("{}", ScrollBarPosition::Right);
    if (tryLoadChild(_usedKeys, _doc, basePath, "scrollbar.position", strValue))
    {
        auto const literal = toLower(strValue);
        if (literal == "left")
            profile.scrollbarPosition = ScrollBarPosition::Left;
        else if (literal == "right")
            profile.scrollbarPosition = ScrollBarPosition::Right;
        else if (literal == "hidden")
            profile.scrollbarPosition = ScrollBarPosition::Hidden;
        else
            errorlog()("Invalid value for config entry {}: {}",
                       "scrollbar.position",
                       strValue);
    }
    tryLoadChild(_usedKeys, _doc, basePath, "scrollbar.hide_in_alt_screen", profile.hideScrollbarInAltScreen);

    tryLoadChild(_usedKeys, _doc, basePath, "history.auto_scroll_on_update", profile.autoScrollOnUpdate);
    tryLoadChild(_usedKeys, _doc, basePath, "history.scroll_multiplier", profile.historyScrollMultiplier);

    float floatValue = 1.0;
    tryLoadChild(_usedKeys, _doc, basePath, "background.opacity", floatValue);
    profile.backgroundOpacity = (terminal::Opacity)(static_cast<unsigned>(255 * clamp(floatValue, 0.0f, 1.0f)));
    tryLoadChild(_usedKeys, _doc, basePath, "background.blur", profile.backgroundBlur);

    strValue = "dotted-underline"; // TODO: fmt::format("{}", profile.hyperlinkDecoration.normal);
    tryLoadChild(_usedKeys, _doc, basePath, "hyperlink_decoration.normal", strValue);
    if (auto const pdeco = terminal::renderer::to_decorator(strValue); pdeco.has_value())
        profile.hyperlinkDecoration.normal = *pdeco;

    strValue = "underline"; // TODO: fmt::format("{}", profile.hyperlinkDecoration.hover);
    tryLoadChild(_usedKeys, _doc, basePath, "hyperlink_decoration.hover", strValue);
    if (auto const pdeco = terminal::renderer::to_decorator(strValue); pdeco.has_value())
        profile.hyperlinkDecoration.hover = *pdeco;

    strValue = "block";
    tryLoadChild(_usedKeys, _doc, basePath, "cursor.shape", strValue);
    profile.cursorShape = terminal::makeCursorShape(strValue);

    bool boolValue = profile.cursorDisplay == terminal::CursorDisplay::Blink;
    tryLoadChild(_usedKeys, _doc, basePath, "cursor.blinking", boolValue);
    profile.cursorDisplay = boolValue ? terminal::CursorDisplay::Blink : terminal::CursorDisplay::Steady;

    unsigned uintValue = profile.cursorBlinkInterval.count();
    tryLoadChild(_usedKeys, _doc, basePath, "cursor.blinking_interval", uintValue);
    profile.cursorBlinkInterval = chrono::milliseconds(uintValue);

    return profile;
}

/**
 * @return success or failure of loading the config file.
 */
void loadConfigFromFile(Config& _config, FileSystem::path const& _fileName)
{
    _config.backingFilePath = _fileName;
    createFileIfNotExists(_config.backingFilePath);
    auto usedKeys = UsedKeys{};

    YAML::Node doc = YAML::LoadFile(_fileName.string());

    tryLoadValue(usedKeys, doc, "word_delimiters", _config.wordDelimiters);

    if (auto opt = parseModifier(usedKeys,
                "bypass_mouse_protocol_modifier",
                doc["bypass_mouse_protocol_modifier"]); opt.has_value())
        _config.bypassMouseProtocolModifier = opt.value();

    if (doc["on_mouse_select"].IsDefined())
    {
        auto const value = toUpper(doc["on_mouse_select"].as<string>());
        auto constexpr mappings = array{
            pair{"COPYTOCLIPBOARD", SelectionAction::CopyToClipboard},
            pair{"COPYTOSELECTIONCLIPBOARD", SelectionAction::CopyToSelectionClipboard},
            pair{"NOTHING", SelectionAction::Nothing},
        };
        bool found = false;
        for (auto const& mapping: mappings)
            if (mapping.first == value)
            {
                _config.onMouseSelection = mapping.second;
                usedKeys.emplace("on_mouse_select");
                found = true;
                break;
            }
        if (!found)
            errorlog()("Invalid action specified for on_mouse_select: {}.", value);
    }

    auto constexpr KnownExperimentalFeatures = array<string_view, 0>{
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
                errorlog()("Unknown experimental feature tag: {}.", key);
                continue;
            }

            usedKeys.emplace("experimental." + x.first.as<string>());
            if (!x.second.as<bool>())
                continue;

            errorlog()("Enabling experimental feature {}.", key);
            _config.experimentalFeatures.insert(key);
        }
    }

    tryLoadValue(usedKeys, doc, "spawn_new_process", _config.spawnNewProcess);

    tryLoadValue(usedKeys, doc, "images.sixel_scrolling", _config.sixelScrolling);
    tryLoadValue(usedKeys, doc, "images.sixel_cursor_conformance", _config.sixelCursorConformance);
    tryLoadValue(usedKeys, doc, "images.sixel_register_count", _config.maxImageColorRegisters);
    tryLoadValue(usedKeys, doc, "images.max_width", _config.maxImageSize.width);
    tryLoadValue(usedKeys, doc, "images.max_height", _config.maxImageSize.height);

    if (auto colorschemes = doc["color_schemes"]; colorschemes)
    {
        usedKeys.emplace("color_schemes");
        for (auto i = colorschemes.begin(); i != colorschemes.end(); ++i)
        {
            auto const name = i->first.as<string>();
            auto const path = "color_schemes." + name;
            _config.colorschemes[name] = loadColorScheme(usedKeys, path, i->second);
        }
    }

    tryLoadValue(usedKeys, doc, "read_buffer_size", _config.ptyReadBufferSize);

    tryLoadValue(usedKeys, doc, "reflow_on_resize", _config.reflowOnResize);

    if (auto profiles = doc["profiles"]; profiles)
    {
        usedKeys.emplace("profiles");
        for (auto i = profiles.begin(); i != profiles.end(); ++i)
        {
            auto const name = i->first.as<string>();
            usedKeys.emplace("profiles." + name);
            _config.profiles[name] = loadTerminalProfile(usedKeys, doc, name, _config.colorschemes);
        }
    }

    // TODO: If there is only one profile, prefill default_profile with that name.
    // TODO: If there are more than one profile, prefill with the top-most one.
    tryLoadValue(usedKeys, doc, "default_profile", _config.defaultProfileName);
    if (!_config.defaultProfileName.empty() && _config.profile(_config.defaultProfileName) == nullptr)
    {
        errorlog()("default_profile \"{}\" not found in profiles list.",
                   escape(_config.defaultProfileName));
    }

	if (auto mapping = doc["input_mapping"]; mapping)
    {
        usedKeys.emplace("input_mapping");
		if (mapping.IsSequence())
			for (size_t i = 0; i < mapping.size(); ++i)
            {
                auto prefix = fmt::format("{}.{}", "input_mapping", i);
				parseInputMapping(usedKeys, prefix, _config, mapping[i]);
            }
    }

    checkForSuperfluousKeys(doc, usedKeys);
}

optional<std::string> readConfigFile(std::string const& _filename)
{
    for (FileSystem::path const& prefix: configHomes("contour"))
        if (auto text = readFile(prefix / _filename); text.has_value())
            return text;

    return nullopt;
}

std::optional<ShaderConfig> Config::loadShaderConfig(ShaderClass _shaderClass)
{
    auto const& defaultConfig = opengl::defaultShaderConfig(_shaderClass);
    auto const basename = to_string(_shaderClass);

    auto const vertText = [&]() -> pair<string, string> {
        auto const fileName = basename + ".vert";
        if (auto content = readConfigFile(fileName); content.has_value())
            return {*content, fileName};
        else
            return {defaultConfig.vertexShader, defaultConfig.vertexShaderFileName};
    }();

    auto const fragText = [&]() -> pair<string, string> {
        auto const fileName = basename + ".frag";
        if (auto content = readConfigFile(fileName); content.has_value())
            return {*content, fileName};
        else
            return {defaultConfig.fragmentShader, defaultConfig.fragmentShaderFileName};
    }();

    auto const prependVersionPragma = [&](string const& _code) {
        if (QOpenGLContext::currentContext()->isOpenGLES())
            return R"(#version 300 es
precision mediump int;
precision mediump float;
precision mediump sampler2DArray;
#line 1
)" + _code;
            //return "#version 300 es\n#line 1\n" + _code;
        else
            return "#version 330\n#line 1\n" + _code;
    };

    return {ShaderConfig{
        prependVersionPragma(vertText.first),
        prependVersionPragma(fragText.first),
        vertText.second,
        fragText.second
    }};
}

} // namespace contour
