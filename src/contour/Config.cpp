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

#include <crispy/overloaded.h>
#include <crispy/stdfs.h>
#include <crispy/debuglog.h>

#include <yaml-cpp/yaml.h>
#include <yaml-cpp/ostream_wrapper.h>

#include <QtGui/QOpenGLContext>

#include <array>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#endif

auto constexpr MinimumFontSize = text::font_size{ 8.0 };

namespace contour::config {

namespace {
    auto const ConfigTag = crispy::debugtag::make("config", "Logs configuration file loading.");
}

using namespace std;
using actions::Action;

namespace
{
    template <typename String>
    inline std::string toLower(String const& _value)
    {
        std::string result;
        result.reserve(_value.size());
        std::transform(
            begin(_value),
            end(_value),
            back_inserter(result),
            [](auto ch) { return std::tolower(ch); }
        );
        return result;
    }

    template <typename String>
    inline std::string toUpper(String const& _value)
    {
        std::string result;
        result.reserve(_value.size());
        std::transform(
            begin(_value),
            end(_value),
            back_inserter(result),
            [](auto ch) { return std::toupper(ch); }
        );
        return result;
    }

    string parseEscaped(string const& _value) {
        string out;
        out.reserve(_value.size());

        enum class State { Text, Escape, Octal1, Octal2, Hex1, Hex2 };
        State state = State::Text;
        char buf[3] = {};

        for (size_t i = 0; i < _value.size(); ++i)
        {
            switch (state)
            {
                case State::Text:
                    if (_value[i] == '\\')
                        state = State::Escape;
                    else
                        out.push_back(_value[i]);
                    break;
                case State::Escape:
                    if (_value[i] == '0')
                        state = State::Octal1;
                    else if (_value[i] == 'x')
                        state = State::Hex1;
                    else if (_value[i] == 'e')
                    {
                        state = State::Text;
                        out.push_back('\033');
                    }
                    else if (_value[i] == '\\')
                    {
                        out.push_back('\\');
                        state = State::Text;
                    }
                    else
                    {
                        // Unknown escape sequence, so just continue as text.
                        out.push_back('\\');
                        out.push_back(_value[i]);
                        state = State::Text;
                    }
                    break;
                case State::Octal1:
                    buf[0] = _value[i];
                    state = State::Octal2;
                    break;
                case State::Octal2:
                    buf[1] = _value[i];
                    out.push_back(static_cast<char>(strtoul(buf, nullptr, 8)));
                    state = State::Text;
                    break;
                case State::Hex1:
                    buf[0] = _value[i];
                    state = State::Hex2;
                    break;
                case State::Hex2:
                    buf[1] = _value[i];
                    out.push_back(static_cast<char>(strtoul(buf, nullptr, 16)));
                    state = State::Text;
                    break;
            }
        }

        return out;
    }
}

vector<actions::Action> const* apply(InputMappings const& _mappings,
                                     terminal::KeyInputEvent const& _event)
{
    if (auto const i = _mappings.keyMappings.find(_event); i != _mappings.keyMappings.end())
        return &i->second;
    return nullptr;
}

vector<actions::Action> const* apply(InputMappings const& _mappings,
                                     terminal::CharInputEvent const& _event)
{
    if (auto const i = _mappings.charMappings.find(_event); i != _mappings.charMappings.end())
        return &i->second;
    return nullptr;
}


vector<actions::Action> const* apply(InputMappings const& _mappings,
                                     terminal::MousePressEvent const& _event)
{
    if (auto const i = _mappings.mouseMappings.find(_event); i != _mappings.mouseMappings.end())
        return &i->second;
    return nullptr;
}

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

template <typename T>
bool softLoadValue(YAML::Node const& _node, string const& _name, T& _store)
{
    if (auto value = _node[_name]; value)
    {
        _store = value.as<T>();
        return false;
    }
    return false;
}

template <typename T, typename U>
void softLoadValue(YAML::Node const& _node, string const& _name, T& _store, U const& _default)
{
    if (auto value = _node[_name]; value)
        _store = value.as<T>();
    else
        _store = _default;
}

void softLoadPermission(YAML::Node const& _node, string const& _name, Permission& _out)
{
    if (auto const valueNode = _node[_name]; valueNode && valueNode.IsScalar())
    {
        auto const value = valueNode.as<string>();
        if (value == "allow")
            _out = Permission::Allow;
        else if (value == "deny")
            _out = Permission::Deny;
        else if (value == "ask")
            _out = Permission::Ask;
    }
}

void createFileIfNotExists(FileSystem::path const& _path)
{
    if (!FileSystem::is_regular_file(_path))
        if (auto const ec = createDefaultConfig(_path); ec)
            throw runtime_error{fmt::format("Could not create directory {}. {}",
                                            _path.parent_path().string(),
                                            ec.message())};
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
        pair{"ENTER"sv, (char) C0::CR}, // TODO: should map to Qt::Key_Return instead
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

optional<terminal::Modifier> parseModifier(YAML::Node const& _node)
{
    using terminal::Modifier;
    if (!_node)
        return nullopt;
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

void parseInputMapping(Config& _config, YAML::Node const& _mapping)
{
	using namespace terminal;

	auto const parseAction = [&](YAML::Node const& _parent) -> optional<Action> {
        auto actionOpt = actions::fromString(_parent["action"].as<string>());
        if (!actionOpt)
        {
            cerr << "Unknown action: '" << _parent["action"].as<string>() << '\'' << endl;
            return nullopt;
        }

        auto action = actionOpt.value();

        if (holds_alternative<actions::ChangeProfile>(action))
        {
            if (auto name = _parent["name"]; name.IsScalar())
                return actions::ChangeProfile{name.as<string>()};
            else
                return nullopt;
        }

        if (holds_alternative<actions::NewTerminal>(action))
        {
            if (auto profile = _parent["profile"]; profile && profile.IsScalar())
                return actions::NewTerminal{profile.as<string>()};
            else
                return action;
        }

        if (holds_alternative<actions::ReloadConfig>(action))
        {
            if (auto profileName = _parent["profile"]; profileName.IsScalar())
                return actions::ReloadConfig{profileName.as<string>()};
            else
                return action;
        }

        if (holds_alternative<actions::SendChars>(action))
        {
            if (auto chars = _parent["chars"]; chars.IsScalar())
                return actions::SendChars{parseEscaped(chars.as<string>())};
            else
                return nullopt;
        }

        if (holds_alternative<actions::WriteScreen>(action))
        {
            if (auto chars = _parent["chars"]; chars.IsScalar())
                return actions::WriteScreen{parseEscaped(chars.as<string>())};
            else
                return nullopt;
        }

		return action;
	};

    auto const parseMouseEvent = [](YAML::Node const& _node, Modifier _mods) -> pair<optional<MousePressEvent>, bool> {
        if (!_node)
            return make_pair(nullopt, true);
        else if (!_node.IsScalar())
            return make_pair(nullopt, false);
        else
        {
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
                    return make_pair(MousePressEvent{mapping.second, _mods}, true);
            return make_pair(nullopt, true);
        }
    };

	auto const makeKeyEvent = [&](YAML::Node const& _node, Modifier _mods) -> pair<optional<variant<terminal::KeyInputEvent, terminal::CharInputEvent>>, bool> {
        if (!_node)
            return make_pair(nullopt, false);

        if (!_node.IsScalar())
            return make_pair(nullopt, true);

        auto const input = parseKeyOrChar(_node.as<string>());
        if (!input.has_value())
            return make_pair(nullopt, false);

        if (std::holds_alternative<terminal::Key>(*input))
            return {terminal::KeyInputEvent{std::get<terminal::Key>(*input), _mods}, true};

        return {terminal::CharInputEvent{std::get<char32_t>(*input), _mods}, true};
    };

    auto const action = parseAction(_mapping);
	auto const mods = parseModifier(_mapping["mods"]);
    if (action && mods)
    {
        if (auto const [keyEvent, ok] = makeKeyEvent(_mapping["key"], mods.value()); ok)
        {
            if (keyEvent.has_value())
            {
                if (holds_alternative<KeyInputEvent>(*keyEvent))
                    _config.inputMappings.keyMappings[get<KeyInputEvent>(*keyEvent)].emplace_back(*action);
                else
                    _config.inputMappings.charMappings[get<CharInputEvent>(*keyEvent)].emplace_back(*action);
            }
        }
        else if (auto const [mouseEvent, ok] = parseMouseEvent(_mapping["mouse"], mods.value()); ok)
        {
            if (mouseEvent.has_value())
                _config.inputMappings.mouseMappings[*mouseEvent].emplace_back(*action);
        }
        else
        {
            // TODO: log error: invalid key mapping at: _mapping.sourceLocation()
        }
    }
}

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

terminal::ColorPalette loadColorScheme(YAML::Node const& _node)
{
    auto colors = terminal::ColorPalette{};

    using terminal::RGBColor;
    if (auto def = _node["default"]; def)
    {
        if (auto fg = def["foreground"]; fg)
            colors.defaultForeground = fg.as<string>();
        if (auto bg = def["background"]; bg)
            colors.defaultBackground = bg.as<string>();
    }

    if (auto def = _node["selection"]; def && def.IsMap())
    {
        if (auto fg = def["foreground"]; fg && fg.IsScalar())
            colors.selectionForeground.emplace(fg.as<string>());
        else
            colors.selectionForeground.reset();

        if (auto bg = def["background"]; bg && bg.IsScalar())
            colors.selectionBackground.emplace(bg.as<string>());
        else
            colors.selectionBackground.reset();
    }

    if (auto cursor = _node["cursor"]; cursor && cursor.IsScalar() && !cursor.as<string>().empty())
        colors.cursor = cursor.as<string>();

    if (auto hyperlink = _node["hyperlink_decoration"]; hyperlink)
    {
        if (auto color = hyperlink["normal"]; color && color.IsScalar() && !color.as<string>().empty())
            colors.hyperlinkDecoration.normal = color.as<string>();

        if (auto color = hyperlink["hover"]; color && color.IsScalar() && !color.as<string>().empty())
            colors.hyperlinkDecoration.hover = color.as<string>();
    }

    auto const loadColorMap = [&](YAML::Node const& _node, size_t _offset) {
        if (_node)
        {
            if (_node.IsMap())
            {
                auto const assignColor = [&](size_t _index, string const& _name) {
                    if (auto nodeValue = _node[_name]; nodeValue)
                    {
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
            else if (_node.IsSequence())
            {
                for (size_t i = 0; i < _node.size() && i < 8; ++i)
                    if (_node.IsScalar())
                        colors.palette[i] = _node[i].as<long>();
                    else
                        colors.palette[i] = _node[i].as<string>();
            }
        }
    };

    loadColorMap(_node["normal"], 0);
    loadColorMap(_node["bright"], 8);
    // TODO: color palette from 16..255
    // TODO: dim _node (maybe put them into the palette at 256..(256+8)?)

    return colors;
}

void softLoadFont(YAML::Node const& _node, string_view _key, text::font_description& _store)
{
    auto node = _node[string(_key)];
    if (!node)
        return;

    if (node.IsScalar())
    {
        _store.familyName = node.as<string>();
    }
    else if (node.IsMap())
    {
        if (node["family"].IsScalar())
            _store.familyName = node["family"].as<string>();

        if (node["slant"].IsScalar())
        {
            if (auto const p = text::make_font_slant(node["slant"].as<string>()))
                _store.slant = p.value();
        }

        if (node["weight"].IsScalar())
        {
            if (auto const p = text::make_font_weight(node["weight"].as<string>()))
                _store.weight = p.value();
        }

        if (node["features"].IsSequence())
        {
            // TODO: array of strings into _store.features
        }
    }
}

bool sanitizeRange(std::reference_wrapper<int> _value, int _min, int _max)
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

TerminalProfile loadTerminalProfile(YAML::Node const& _node,
                                    unordered_map<string, terminal::ColorPalette> const& _colorschemes)
{
    auto profile = TerminalProfile{};

    if (auto colors = _node["colors"]; colors)
    {
        if (colors.IsMap())
            profile.colors = loadColorScheme(colors);
        else if (auto i = _colorschemes.find(colors.as<string>()); i != _colorschemes.end())
            profile.colors = i->second;
        else
            cerr << fmt::format("scheme '{}' not found.", colors.as<string>()) << '\n';
    }
    else
        cerr << fmt::format("No colors section found.") << '\n';

    softLoadValue(_node, "shell", profile.shell.program);
    if (profile.shell.program.empty())
        profile.shell.program = terminal::Process::loginShell();

    softLoadValue(_node, "maximized", profile.maximized, false);
    softLoadValue(_node, "fullscreen", profile.fullscreen, false);

    if (auto const value = _node["refresh_rate"])
    {
        if (value.IsScalar() && value.as<string>() == "auto")
            profile.refreshRate = 0.0;
        else
            profile.refreshRate = value.as<double>();
    }

    if (auto args = _node["arguments"]; args && args.IsSequence())
        for (auto const& argNode : args)
            profile.shell.arguments.emplace_back(argNode.as<string>());

    if (auto wd = _node["initial_working_directory"]; wd && wd.IsScalar())
    {
        string const& value = wd.Scalar();
        if (value.empty())
            profile.shell.workingDirectory = FileSystem::current_path();
        else if (value[0] != '~')
            profile.shell.workingDirectory = FileSystem::path(value);
        else
        {
            bool const delim = value.size() >= 2 && (value[1] == '/' || value[1] == '\\');
            auto const subPath = FileSystem::path(value.substr(delim ? 2 : 1));
            profile.shell.workingDirectory = terminal::Process::homeDirectory() / subPath;
        }
    }
    else
        profile.shell.workingDirectory = FileSystem::current_path();

    profile.shell.env["TERMINAL_NAME"] = "contour";
    profile.shell.env["TERMINAL_VERSION_TRIPLE"] = fmt::format("{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH);
    profile.shell.env["TERMINAL_VERSION_STRING"] = CONTOUR_VERSION_STRING;

    if (auto env = _node["environment"]; env)
    {
        for (auto i = env.begin(); i != env.end(); ++i)
        {
            auto const name = i->first.as<string>();
            auto const value = i->second.as<string>();
            profile.shell.env[name] = value;
        }
    }

    // force some default env
    if (profile.shell.env.find("TERM") == profile.shell.env.end())
        profile.shell.env["TERM"] = "xterm-256color";
    if (profile.shell.env.find("COLORTERM") == profile.shell.env.end())
        profile.shell.env["COLORTERM"] = "truecolor";

    if (auto const terminalId = _node["terminal_id"]; terminalId)
    {
        auto const terminalIdStr = terminalId.as<string>();
        if (auto const idOpt = stringToVTType(terminalIdStr))
            profile.terminalId = idOpt.value();
        else
            errorlog().write("Invalid Terminal ID \"{}\", specified",
                             terminalIdStr);
    }

    if (auto terminalSize = _node["terminal_size"]; terminalSize)
    {
        softLoadValue(terminalSize, "columns", profile.terminalSize.width);
        softLoadValue(terminalSize, "lines", profile.terminalSize.height);

        auto constexpr MinimalTerminalSize = crispy::Size{3, 3};
        auto constexpr MaximumTerminalSize = crispy::Size{300, 200};

        if (!sanitizeRange(ref(profile.terminalSize.width), MinimalTerminalSize.width, MaximumTerminalSize.width))
            errorlog().write(
                "Terminal width {} out of bounds. Should be between {} and {}.",
                profile.terminalSize.width, MinimalTerminalSize.width, MaximumTerminalSize.width
            );

        if (!sanitizeRange(ref(profile.terminalSize.height), MinimalTerminalSize.height, MaximumTerminalSize.height))
            errorlog().write(
                "Terminal height {} out of bounds. Should be between {} and {}.",
                profile.terminalSize.height, MinimalTerminalSize.height, MaximumTerminalSize.height
            );
    }

    if (auto const permissions = _node["permissions"]; permissions && permissions.IsMap())
    {
        softLoadPermission(permissions, "capture_buffer", profile.permissions.captureBuffer);
        softLoadPermission(permissions, "change_font", profile.permissions.changeFont);
    }

    if (auto fonts = _node["font"]; fonts)
    {
        softLoadValue(fonts, "size", profile.fonts.size.pt);
        if (profile.fonts.size < MinimumFontSize)
        {
            errorlog().write("Invalid font size {} set in config file. Minimum value is {}.",
                             profile.fonts.size, MinimumFontSize);
            profile.fonts.size = MinimumFontSize;
        }

        softLoadValue(fonts, "dpi_scale", profile.fonts.dpiScale);

        if (auto textShaping = fonts["text_shaping"]; textShaping)
        {
            if (auto methodNode = textShaping["method"]; methodNode)
            {
                auto const methodStr = methodNode.as<string>();
                if (methodStr == "simple")
                    profile.fonts.textShapingMethod = terminal::renderer::TextShapingMethod::Simple;
                else if (methodStr == "complex")
                    profile.fonts.textShapingMethod = terminal::renderer::TextShapingMethod::Complex;
                else
                    errorlog().write("Unknown text shaping method: {}", methodStr);
            }
        }

        bool onlyMonospace = true;
        softLoadValue(fonts, "only_monospace", onlyMonospace, true);

        profile.fonts.regular.familyName = "regular";
        profile.fonts.regular.spacing = text::font_spacing::mono;
        profile.fonts.regular.force_spacing = onlyMonospace;
        softLoadFont(fonts, "regular", profile.fonts.regular);

        profile.fonts.bold = profile.fonts.regular;
        profile.fonts.bold.weight = text::font_weight::bold;
        softLoadFont(fonts, "bold", profile.fonts.bold);

        profile.fonts.italic = profile.fonts.regular;
        profile.fonts.italic.slant = text::font_slant::italic;
        softLoadFont(fonts, "italic", profile.fonts.italic);

        profile.fonts.boldItalic = profile.fonts.regular;
        profile.fonts.boldItalic.weight = text::font_weight::bold;
        profile.fonts.boldItalic.slant = text::font_slant::italic;
        softLoadFont(fonts, "bold_italic", profile.fonts.boldItalic);

        profile.fonts.emoji.familyName = "emoji";
        profile.fonts.emoji.spacing = text::font_spacing::mono;
        softLoadFont(fonts, "emoji", profile.fonts.emoji);

        string renderModeStr;
        softLoadValue(fonts, "render_mode", renderModeStr);
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
            errorlog().write("Invalid render_mode \"{}\" in configuration.", renderModeStr);
        debuglog(ConfigTag).write("Using render mode: {}", profile.fonts.renderMode);
    }

    if (auto history = _node["history"]; history)
    {
        if (auto limit = history["limit"]; limit)
        {
            if (limit.as<int>() < 0)
                profile.maxHistoryLineCount = nullopt;
            else
                profile.maxHistoryLineCount = limit.as<size_t>();
        }

        softLoadValue(history, "auto_scroll_on_update", profile.autoScrollOnUpdate);
        softLoadValue(history, "scroll_multiplier", profile.historyScrollMultiplier);
    }

    if (auto background = _node["background"]; background)
    {
        if (auto opacity = background["opacity"]; opacity)
            profile.backgroundOpacity =
                (terminal::Opacity)(static_cast<unsigned>(255 * clamp(opacity.as<float>(), 0.0f, 1.0f)));
        softLoadValue(background, "blur", profile.backgroundBlur);
    }

    if (auto deco = _node["hyperlink_decoration"]; deco)
    {
        if (auto normal = deco["normal"]; normal && normal.IsScalar())
            if (auto const pdeco = terminal::renderer::to_decorator(normal.as<string>()); pdeco.has_value())
                profile.hyperlinkDecoration.normal = *pdeco;

        if (auto hover = deco["hover"]; hover && hover.IsScalar())
            if (auto const pdeco = terminal::renderer::to_decorator(hover.as<string>()); pdeco.has_value())
                profile.hyperlinkDecoration.hover = *pdeco;
    }

    if (auto cursor = _node["cursor"]; cursor)
    {
        if (auto shape = cursor["shape"]; shape)
            profile.cursorShape = terminal::makeCursorShape(shape.as<string>());

        bool blinking = false;
        softLoadValue(cursor, "blinking", blinking);
        profile.cursorDisplay = blinking ? terminal::CursorDisplay::Blink : terminal::CursorDisplay::Steady;

        if (cursor["blinking_interval"].IsDefined())
            profile.cursorBlinkInterval = chrono::milliseconds(cursor["blinking_interval"].as<int>());
    }

    return profile;
}

/**
 * @return success or failure of loading the config file.
 */
void loadConfigFromFile(Config& _config, FileSystem::path const& _fileName)
{
    _config.backingFilePath = _fileName;
    createFileIfNotExists(_config.backingFilePath);

    YAML::Node doc = YAML::LoadFile(_fileName.string());

    softLoadValue(doc, "word_delimiters", _config.wordDelimiters);

    if (auto opt = parseModifier(doc["bypass_mouse_protocol_modifier"]); opt.has_value())
        _config.bypassMouseProtocolModifier = opt.value();

    auto constexpr KnownExperimentalFeatures = array{
        "tcap"sv
    };

    if (auto experimental = doc["experimental"]; experimental.IsMap())
    {
        for (auto const& x: experimental)
        {
            auto const key = x.first.as<string>();
            if (crispy::count(KnownExperimentalFeatures, key) == 0)
            {
                errorlog().write("Unknown experimental feature tag: {}.", key);
                continue;
            }

            if (!x.second.as<bool>())
                continue;

            errorlog().write("Enabling experimental feature {}.", key);
            _config.experimentalFeatures.insert(key);
        }
    }

    if (auto images = doc["images"]; images)
    {
        softLoadValue(images, "sixel_scrolling", _config.sixelScrolling);
        softLoadValue(images, "sixel_cursor_conformance", _config.sixelCursorConformance);
        softLoadValue(images, "sixel_register_count", _config.maxImageColorRegisters);
        softLoadValue(images, "max_width", _config.maxImageSize.width);
        softLoadValue(images, "max_height", _config.maxImageSize.height);
    }

    if (auto scrollbar = doc["scrollbar"]; scrollbar)
    {
        if (auto value = scrollbar["position"]; value)
        {
            auto const literal = toLower(value.as<string>());
            if (literal == "left")
                _config.scrollbarPosition = ScrollBarPosition::Left;
            else if (literal == "right")
                _config.scrollbarPosition = ScrollBarPosition::Right;
            else if (literal == "hidden")
                _config.scrollbarPosition = ScrollBarPosition::Hidden;
            else
                throw std::runtime_error("Invalid value in scrollbar_position. Should be one of: left, right, hidden.");
        }

        if (auto value = scrollbar["hide_in_alt_screen"]; value)
            _config.hideScrollbarInAltScreen = value.as<bool>();
    }

    if (auto profiles = doc["color_schemes"]; profiles)
    {
        for (auto i = profiles.begin(); i != profiles.end(); ++i)
        {
            auto const name = i->first.as<string>();
            _config.colorschemes[name] = loadColorScheme(i->second);
        }
    }

    softLoadValue(doc, "read_buffer_size", _config.ptyReadBufferSize);

    if (auto profiles = doc["profiles"]; profiles)
    {
        for (auto i = profiles.begin(); i != profiles.end(); ++i)
        {
            auto const name = i->first.as<string>();
            _config.profiles[name] = loadTerminalProfile(i->second, _config.colorschemes);
        }
    }

    softLoadValue(doc, "default_profile", _config.defaultProfileName);
    if (!_config.defaultProfileName.empty() && _config.profile(_config.defaultProfileName) == nullptr)
    {
        cerr << fmt::format("default_profile \"{}\" not found in profiles list.",
                            _config.defaultProfileName) << '\n';
    }

	if (auto mapping = doc["input_mapping"]; mapping)
    {
		if (mapping.IsSequence())
			for (size_t i = 0; i < mapping.size(); ++i)
				parseInputMapping(_config, mapping[i]);
    }

    auto const logMappings = [](auto const& _mapping) {
        for (auto const& m: _mapping)
            for (auto const& a: m.second)
                debuglog(ConfigTag).write("Parsed input mapping: {} {}", m.first, a);
    };
    logMappings(_config.inputMappings.keyMappings);
    logMappings(_config.inputMappings.charMappings);
    logMappings(_config.inputMappings.mouseMappings);
}

optional<std::string> readConfigFile(std::string const& _filename)
{
    for (FileSystem::path const& prefix : configHomes("contour"))
    {
        FileSystem::path path = prefix / _filename;
        if (!FileSystem::exists(path))
            continue;

        auto ifs = ifstream(path.string());
        if (!ifs.good())
            continue;

        auto const size = FileSystem::file_size(path);
        auto text = string{};
        text.resize(size);
        ifs.read(text.data(), size);
        return {text};
    }
    return nullopt;
}

std::optional<ShaderConfig> Config::loadShaderConfig(ShaderClass _shaderClass)
{
    auto const& defaultConfig = terminal::renderer::opengl::defaultShaderConfig(_shaderClass);
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
