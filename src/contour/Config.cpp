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

#include <yaml-cpp/yaml.h>
#include <yaml-cpp/ostream_wrapper.h>

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

namespace contour::config {

using namespace std;
using actions::Action;

namespace {
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

    auto toQtKeyboardModifier(terminal::Modifier _mods) -> Qt::KeyboardModifiers {
        Qt::KeyboardModifiers mods;
        if (_mods.shift())
            mods.setFlag(Qt::ShiftModifier);
        if (_mods.alt())
            mods.setFlag(Qt::AltModifier);
        if (_mods.control())
            mods.setFlag(Qt::ControlModifier);
        if (_mods.meta())
            mods.setFlag(Qt::MetaModifier);
        return mods;
    };

    QKeySequence toQtKeySequence(terminal::CharInputEvent _char) {
        auto const mods = toQtKeyboardModifier(_char.modifier);
        using terminal::ControlCode::C0;
        switch (static_cast<C0>(_char.value))
        {
            case C0::CR:
                return QKeySequence(Qt::Key_Return | mods);
            default:
                return QKeySequence(_char.value | mods);
        }
    };

    QKeySequence toQtKeySequence(terminal::KeyInputEvent const& _key) {
        using terminal::Key;

        static auto constexpr mapping = array{
            pair{Key::Insert, Qt::Key_Insert},
            pair{Key::Delete, Qt::Key_Delete},
            pair{Key::RightArrow, Qt::Key_Right},
            pair{Key::LeftArrow, Qt::Key_Left},
            pair{Key::DownArrow, Qt::Key_Down},
            pair{Key::UpArrow, Qt::Key_Up},
            pair{Key::PageDown, Qt::Key_PageDown},
            pair{Key::PageUp, Qt::Key_PageUp},
            pair{Key::Home, Qt::Key_Home},
            pair{Key::End, Qt::Key_End},
            pair{Key::F1, Qt::Key_F1},
            pair{Key::F2, Qt::Key_F2},
            pair{Key::F3, Qt::Key_F3},
            pair{Key::F4, Qt::Key_F4},
            pair{Key::F5, Qt::Key_F5},
            pair{Key::F6, Qt::Key_F6},
            pair{Key::F7, Qt::Key_F7},
            pair{Key::F8, Qt::Key_F8},
            pair{Key::F9, Qt::Key_F9},
            pair{Key::F10, Qt::Key_F10},
            pair{Key::F11, Qt::Key_F11},
            pair{Key::F12, Qt::Key_F12},
            // todo: F13..F25
            // TODO: NumPad
            // pair{Key::Numpad_0, Qt::Key_0},
            // pair{Key::Numpad_1, Qt::Key_1},
            // pair{Key::Numpad_2, Qt::Key_2},
            // pair{Key::Numpad_3, Qt::Key_3},
            // pair{Key::Numpad_4, Qt::Key_4},
            // pair{Key::Numpad_5, Qt::Key_5},
            // pair{Key::Numpad_6, Qt::Key_6},
            // pair{Key::Numpad_7, Qt::Key_7},
            // pair{Key::Numpad_8, Qt::Key_8},
            // pair{Key::Numpad_9, Qt::Key_9},
            // pair{Key::Numpad_Decimal, Qt::Key_Period},
            // pair{Key::Numpad_Divide, Qt::Key_Slash},
            // pair{Key::Numpad_Multiply, Qt::Key_Asterisk},
            // pair{Key::Numpad_Subtract, Qt::Key_Minus},
            // pair{Key::Numpad_Add, Qt::Key_Plus},
            // pair{Key::Numpad_Enter, Qt::Key_Enter},
            // pair{Key::Numpad_Equal, Qt::Key_Equal},
        };

        if (auto i = find_if(begin(mapping), end(mapping), [_key](auto const& x) { return x.first == _key.key; }); i != end(mapping))
            return QKeySequence{static_cast<int>(i->second) | static_cast<int>(toQtKeyboardModifier(_key.modifier)) };

        throw std::invalid_argument(fmt::format("Unsupported input Key. {}", _key.key));
    };
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

void createFileIfNotExists(FileSystem::path const& _path)
{
    if (!FileSystem::is_regular_file(_path))
        if (auto const ec = createDefaultConfig(_path); ec)
            throw runtime_error{fmt::format("Could not create directory {}. {}",
                                            _path.parent_path().string(),
                                            ec.message())};
}

error_code createDefaultConfig(FileSystem::path const& _path)
{
    FileSystemError ec;
    FileSystem::create_directories(_path.parent_path(), ec);
    if (ec)
        return ec;

    ofstream{_path.string(), ios::binary | ios::trunc}.write(
        (char const*) contour::default_config_yaml.data(),
        contour::default_config_yaml.size()
    );

    return error_code{};
}

template <typename T>
inline auto mapAction(std::string_view _name)
{
	return pair{_name, Action{T{}}};
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

	auto const parseModifier = [&](YAML::Node const& _node) -> optional<terminal::Modifier> {
		if (!_node)
			return Modifier::None;
		else if (_node.IsScalar())
			return parseModifierKey(_node.as<string>());
		else if (_node.IsSequence())
		{
			terminal::Modifier mods;
			for (size_t i = 0; i < _node.size(); ++i)
			{
				if (!_node[i].IsScalar())
					return nullopt;
				else if (auto const mod = parseModifierKey(_node[i].as<string>()); mod)
					mods |= *mod;
				else
					return nullopt;
			}
			return mods;
		}
		else
			return nullopt;
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

	auto const makeKeyEvent = [&](YAML::Node const& _node, Modifier _mods) -> pair<optional<terminal::InputEvent>, bool> {
        if (!_node)
            return make_pair(nullopt, false);
        else if (!_node.IsScalar())
            return make_pair(nullopt, true);
        else if (auto const input = parseKeyOrChar(_node.as<string>()); input.has_value())
        {
            return make_pair(terminal::InputEvent{visit(overloaded{
                [&](terminal::Key _key) -> terminal::InputEvent {
                    return terminal::KeyInputEvent{_key, _mods};
                },
                [&](char32_t _ch) -> terminal::InputEvent {
                    return terminal::CharInputEvent{static_cast<char32_t>(toupper(_ch)), _mods};
                }
            }, input.value())}, true);
        }

        return make_pair(nullopt, false);
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
                    _config.keyMappings[toQtKeySequence(get<KeyInputEvent>(*keyEvent))].emplace_back(*action);
                else
                    _config.keyMappings[toQtKeySequence(get<CharInputEvent>(*keyEvent))].emplace_back(*action);
            }
        }
        else if (auto const [mouseEvent, ok] = parseMouseEvent(_mapping["mouse"], mods.value()); ok)
        {
            if (mouseEvent.has_value())
                _config.mouseMappings[*mouseEvent].emplace_back(*action);
        }
        else
        {
            // TODO: log error: invalid key mapping at: _mapping.sourceLocation()
        }
    }
}

Config loadConfig(Logger const& _logger)
{
    return loadConfigFromFile((configHome() / "contour.yml").string(), _logger);
}

Config loadConfigFromFile(FileSystem::path const& _fileName, Logger const& _logger)
{
    Config config{};
    loadConfigFromFile(config, _fileName, _logger);
    return config;
}

terminal::ColorProfile loadColorScheme(YAML::Node const& _node)
{
    auto colors = terminal::ColorProfile{};

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

TerminalProfile loadTerminalProfile(YAML::Node const& _node,
                                    unordered_map<string, terminal::ColorProfile> const& _colorschemes,
                                    Logger const& _logger)
{
    auto profile = TerminalProfile{};

    if (auto colors = _node["colors"]; colors)
    {
        if (colors.IsMap())
            profile.colors = loadColorScheme(colors);
        else if (auto i = _colorschemes.find(colors.as<string>()); i != _colorschemes.end())
            profile.colors = i->second;
        else
            _logger(fmt::format("scheme '{}' not found.", colors.as<string>()));
    }
    else
        _logger(fmt::format("No colors section found."));

    softLoadValue(_node, "shell", profile.shell.program);
    if (profile.shell.program.empty())
        profile.shell.program = terminal::Process::loginShell();

    if (auto args = _node["arguments"]; args && args.IsSequence())
        for (auto const& argNode : args)
            profile.shell.arguments.emplace_back(argNode.as<string>());

    if (auto wd = _node["initial_working_directory"]; wd && wd.IsScalar())
    {
        string const& value = wd.Scalar();
        if (value.empty())
            profile.shell.workingDirectory = terminal::Process::homeDirectory();
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
        profile.shell.workingDirectory = terminal::Process::homeDirectory();

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

    if (auto terminalSize = _node["terminal_size"]; terminalSize)
    {
        softLoadValue(terminalSize, "columns", profile.terminalSize.width);
        softLoadValue(terminalSize, "lines", profile.terminalSize.height);
    }

    softLoadValue(_node, "font_size", profile.fontSize);

    if (auto fonts = _node["font"]; fonts)
    {
        auto& regularPattern = profile.fonts.regular.pattern;
        softLoadValue(fonts, "regular", profile.fonts.regular.pattern, "monospace");
        softLoadValue(fonts, "bold", profile.fonts.bold.pattern, regularPattern + ":style=bold");
        softLoadValue(fonts, "italic", profile.fonts.italic.pattern, regularPattern + ":style=italic");
        softLoadValue(fonts, "bold_italic", profile.fonts.boldItalic.pattern, regularPattern + ":style=bold italic");
        softLoadValue(fonts, "emoji", profile.fonts.emoji.pattern, "emoji");
    }

    softLoadValue(_node, "tab_width", profile.tabWidth);

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
            if (auto const pdeco = terminal::view::to_decorator(normal.as<string>()); pdeco.has_value())
                profile.hyperlinkDecoration.normal = *pdeco;

        if (auto hover = deco["hover"]; hover && hover.IsScalar())
            if (auto const pdeco = terminal::view::to_decorator(hover.as<string>()); pdeco.has_value())
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

void loadConfigFromFile(Config& _config,
                        FileSystem::path const& _fileName,
                        Logger const& _logger)
{
    _config.backingFilePath = _fileName;
    createFileIfNotExists(_config.backingFilePath);

    YAML::Node doc = YAML::LoadFile(_fileName.string());

    softLoadValue(doc, "word_delimiters", _config.wordDelimiters);

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

    if (auto profiles = doc["profiles"]; profiles)
    {
        for (auto i = profiles.begin(); i != profiles.end(); ++i)
        {
            auto const name = i->first.as<string>();
            _config.profiles[name] = loadTerminalProfile(i->second, _config.colorschemes, _logger);
        }
    }

    softLoadValue(doc, "default_profile", _config.defaultProfileName);
    if (!_config.defaultProfileName.empty() && _config.profile(_config.defaultProfileName) == nullptr)
    {
        _logger(fmt::format("default_profile \"{}\" not found in profiles list.",
                            _config.defaultProfileName));
    }

	if (auto mapping = doc["input_mapping"]; mapping)
    {
		if (mapping.IsSequence())
			for (size_t i = 0; i < mapping.size(); ++i)
				parseInputMapping(_config, mapping[i]);
    }

    if (auto logging = doc["logging"]; logging)
    {
        if (auto filePath = logging["file"]; filePath)
            _config.logFilePath = {FileSystem::path{filePath.as<string>()}};

        auto constexpr mappings = array{
            pair{"parse_errors", LogMask::ParserError},
            pair{"invalid_output", LogMask::InvalidOutput},
            pair{"unsupported_output", LogMask::UnsupportedOutput},
            pair{"raw_input", LogMask::RawInput},
            pair{"raw_output", LogMask::RawOutput},
            pair{"trace_input", LogMask::TraceInput},
            pair{"trace_output", LogMask::TraceOutput},
        };

        for (auto const& mapping : mappings)
        {
            if (auto value = logging[mapping.first]; value)
            {
                if (value.as<bool>())
                    _config.loggingMask |= mapping.second;
                else
                    _config.loggingMask &= ~mapping.second;
            }
        }
    }
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
    auto const& defaultConfig = terminal::view::defaultShaderConfig(_shaderClass);
    auto const basename = to_string(_shaderClass);

    auto const vertText = [&]() {
        if (auto content = readConfigFile(basename + ".vert"); content.has_value())
            return *content;
        else
            return defaultConfig.vertexShader;
    }();

    auto const fragText = [&]() {
        if (auto content = readConfigFile(basename + ".frag"); content.has_value())
            return *content;
        else
            return defaultConfig.fragmentShader;
    }();

    auto const prependVersionPragma = [&](string const& _code) {
        if (QOpenGLContext::currentContext()->isOpenGLES())
            return "#version 300 es\n#line 1\n" + _code;
        else
            return "#version 330\n#line 1\n" + _code;
    };

    return {ShaderConfig{prependVersionPragma(vertText), prependVersionPragma(fragText)}};
}

} // namespace contour
