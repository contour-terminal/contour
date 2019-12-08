/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include "Flags.h"

#include <terminal/util/overloaded.h>
#include <terminal/util/stdfs.h>
#include <terminal_view/GLCursor.h>
#include <terminal/InputGenerator.h>
#include <terminal/Process.h>

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

using namespace std;

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

optional<int> loadConfigFromCLI(Config& _config, int _argc, char const* _argv[])
{
    util::Flags flags;
    flags.defineBool("help", 'h', "Shows this help and quits.");
    flags.defineBool("version", 'v', "Shows this version and exits.");
    flags.defineString("config", 'c', "PATH", "Specifies path to config file to load from (and save to).", (configHome("contour") / "contour.yml").string());

    flags.parse(_argc, _argv);
    if (flags.getBool("help"))
    {
        cout << "Aero Terminal Emulator.\n"
             << "\n"
             << "Usage:\n"
             << "  contour [OPTIONS ...]\n"
             << "\n"
             << flags.helpText() << endl;
        return {EXIT_SUCCESS};
    }

    if (flags.getBool("version"))
    {
        cout << fmt::format("Aero Terminal, version {}.{}.{}", 0, 1, 0) << endl; // TODO: get from CMake
        return {EXIT_SUCCESS};
    }

    if (flags.isSet("config"))
        loadConfigFromFile(_config, flags.getString("config"));

    return nullopt;
}

template <typename T>
void softLoadValue(YAML::Node const& _node, string const& _name, T& _store)
{
    if (auto value = _node[_name]; value)
        _store = value.as<T>();
}

void createFileIfNotExists(FileSystem::path const& _path)
{
    if (!FileSystem::is_regular_file(_path))
    {
		FileSystemError ec;
		FileSystem::create_directories(_path.parent_path(), ec);
		if (ec)
		{
			throw runtime_error{fmt::format(
					"Could not create directory {}. {}",
					_path.parent_path().string(),
					ec.message())};
		}
		ofstream{_path.string()}.write(
			&contour::default_config_yaml[0],
			contour::default_config_yaml.size()
		);
    }
}

template <typename T>
inline auto mapAction(std::string_view _name)
{
	return pair{_name, Action{T{}}};
}

void parseInputMapping(Config& _config, YAML::Node const& _mapping)
{
	using namespace terminal;

    #if 0 // Example:
    input_mapping:
        - { mods: [Alt], key: Enter, action: ToggleFullscreen }
        - { mods: [Control, Alt],   key: S,   action: ScreenshotVT }
        - { mods: [Control, Shift], key: "+", action: IncreaseFontSize }
        - { mods: [Control, Shift], key: "-", action: DecreaseFontSize }
        - { mods: [Control], mouse: WheelUp, action: IncreaseFontSize }
        - { mods: [Control], mouse: WheelDown, action: DecreaseFontSize }
        - { mods: [Alt], mouse: WheelUp, action: IncreaseOpacity }
        - { mods: [Alt], mouse: WheelDown, action: DecreaseOpacity }
        - [ mods: [Control, Shift], key: V, action: PastClipboard }
        - { mods: [Control, Shift], key: C, action: CopySelection }
        - { mods: [Shift], mouse: RightClick, action: PasteSelection }
    #endif

	auto const parseAction = [&](YAML::Node const& _node, YAML::Node const& _chars) -> optional<Action> {
        if (!_node || !_node.IsScalar())
            return nullopt;

        auto static const mappings = array{
			mapAction<actions::ToggleFullScreen>("ToggleFullscreen"),
            mapAction<actions::IncreaseFontSize>("IncreaseFontSize"),
            mapAction<actions::DecreaseFontSize>("DecreaseFontSize"),
            mapAction<actions::IncreaseOpacity>("IncreaseOpacity"),
            mapAction<actions::DecreaseOpacity>("DecreaseOpacity"),
			mapAction<actions::ScreenshotVT>("ScreenshotVT"),
			mapAction<actions::ScrollOneUp>("ScrollOneUp"),
			mapAction<actions::ScrollOneDown>("ScrollOneDown"),
			mapAction<actions::ScrollUp>("ScrollUp"),
			mapAction<actions::ScrollDown>("ScrollDown"),
			mapAction<actions::ScrollPageUp>("ScrollPageUp"),
			mapAction<actions::ScrollPageDown>("ScrollPageDown"),
			mapAction<actions::ScrollToTop>("ScrollToTop"),
			mapAction<actions::ScrollToBottom>("ScrollToBottom"),
			mapAction<actions::CopySelection>("CopySelection"),
			mapAction<actions::PasteSelection>("PasteSelection"),
			mapAction<actions::PasteClipboard>("PasteClipboard"),
			mapAction<actions::NewTerminal>("NewTerminal"),
			mapAction<actions::OpenConfiguration>("OpenConfiguration"),
			mapAction<actions::Quit>("Quit"),
        };

        auto const name = toLower(_node.as<string>());
        for (auto const& mapping : mappings)
        {
            if (name == toLower(mapping.first))
                return mapping.second;
        }

        if (name == "sendchars")
        {
            if (!_chars || !_chars.IsScalar())
                return nullopt;

            return actions::SendChars{parseEscaped(_chars.as<string>())};
        }

        if (name == "writescreen")
        {
            if (!_chars || !_chars.IsScalar())
                return nullopt;

            return actions::WriteScreen{_chars.as<string>()};
        }

        cerr << "Unknown action: '" << _node.as<string>() << '\'' << endl;

		return nullopt;
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
        else if (auto const input = terminal::parseKeyOrChar(_node.as<string>()); input.has_value())
        {
            return make_pair(terminal::InputEvent{visit(overloaded{
                [&](terminal::Key _key) -> terminal::InputEvent {
                    return terminal::KeyInputEvent{_key, _mods};
                },
                [&](char32_t _ch) -> terminal::InputEvent {
                    return terminal::CharInputEvent{static_cast<char32_t>(tolower(_ch)), _mods};
                }
            }, input.value())}, true);
        }

        return make_pair(nullopt, false);
	};

    auto const action = parseAction(_mapping["action"], _mapping["chars"]);
	auto const mods = parseModifier(_mapping["mods"]);
    if (action && mods)
    {
        if (auto const [keyEvent, ok] = makeKeyEvent(_mapping["key"], mods.value()); ok)
        {
            if (keyEvent.has_value())
                _config.inputMappings[InputEvent{*keyEvent}].emplace_back(*action);
        }
        else if (auto const [mouseEvent, ok] = parseMouseEvent(_mapping["mouse"], mods.value()); ok)
        {
            if (mouseEvent.has_value())
                _config.inputMappings[InputEvent{*mouseEvent}].emplace_back(*action);
        }
        else
        {
            // TODO: log error: invalid key mapping at: _mapping.sourceLocation()
        }
    }
}

void loadConfigFromFile(Config& _config, FileSystem::path const& _fileName)
{
    _config.backingFilePath = _fileName;
    createFileIfNotExists(_config.backingFilePath);

    YAML::Node doc = YAML::LoadFile(_fileName.string());

    softLoadValue(doc, "shell", _config.shell);
	if (_config.shell.empty())
		_config.shell = terminal::Process::loginShell();

    if (auto env = doc["environment"]; env)
    {
        for (auto i = env.begin(); i != env.end(); ++i)
        {
            auto const name = i->first.as<string>();
            auto const value = i->second.as<string>();
            _config.env[name] = value;
        }
    }

    // force some default env
    if (_config.env.find("TERM") == _config.env.end())
        _config.env["TERM"] = "xterm-256color";
    if (_config.env.find("COLORTERM") == _config.env.end())
        _config.env["COLORTERM"] = "truecolor";

    if (auto terminalSize = doc["terminalSize"]; terminalSize)
    {
        softLoadValue(terminalSize, "columns", _config.terminalSize.columns);
        softLoadValue(terminalSize, "lines", _config.terminalSize.rows);
    }

    softLoadValue(doc, "fontSize", _config.fontSize);
    softLoadValue(doc, "fontFamily", _config.fontFamily);
    softLoadValue(doc, "tabWidth", _config.tabWidth);
    softLoadValue(doc, "wordDelimiters", _config.wordDelimiters);

    if (auto history = doc["history"]; history)
    {
        if (auto limit = history["limit"]; limit)
        {
            if (limit.as<int>() < 0)
                _config.maxHistoryLineCount = nullopt;
            else
                _config.maxHistoryLineCount = limit.as<size_t>();
        }

        softLoadValue(history, "autoScrollOnUpdate", _config.autoScrollOnUpdate);
        softLoadValue(history, "scrollMultiplier", _config.historyScrollMultiplier);
    }

    if (auto background = doc["background"]; background)
    {
        if (auto opacity = background["opacity"]; opacity)
            _config.backgroundOpacity =
                (terminal::Opacity)(static_cast<unsigned>(255 * clamp(opacity.as<float>(), 0.0f, 1.0f)));
        softLoadValue(background, "blur", _config.backgroundBlur);
    }

    if (auto cursor = doc["cursor"]; cursor)
    {
        if (auto shape = cursor["shape"]; shape)
            _config.cursorShape = terminal::makeCursorShape(shape.as<string>());

        bool blinking = false;
        softLoadValue(cursor, "blinking", blinking);
        _config.cursorDisplay = blinking ? terminal::CursorDisplay::Blink : terminal::CursorDisplay::Steady;
    }

    if (auto colors = doc["colors"]; colors)
    {
        using terminal::RGBColor;
        if (auto def = colors["default"]; def)
        {
            if (auto fg = def["foreground"]; fg)
                _config.colorProfile.defaultForeground = fg.as<string>();
            if (auto bg = def["background"]; bg)
                _config.colorProfile.defaultBackground = bg.as<string>();
        }

        if (auto selection = colors["selection"]; selection && selection.IsScalar() && !selection.as<string>().empty())
            _config.colorProfile.selection = selection.as<string>();

		if (auto cursor = colors["cursor"]; cursor && cursor.IsScalar() && !cursor.as<string>().empty())
			_config.colorProfile.cursor = cursor.as<string>();

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
                                    _config.colorProfile.palette[_offset + _index] = value;
                                else if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
                                    _config.colorProfile.palette[_offset + _index] = nodeValue.as<unsigned long>();
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
                            _config.colorProfile.palette[i] = _node[i].as<long>();
                        else
                            _config.colorProfile.palette[i] = _node[i].as<string>();
                }
            }
        };

        loadColorMap(colors["normal"], 0);
        loadColorMap(colors["bright"], 8);
        // TODO: color palette from 16..255
        // TODO: dim colors (maybe put them into the palette at 256..(256+8)?)
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
            pair{"parseErrors", LogMask::ParserError},
            pair{"invalidOutput", LogMask::InvalidOutput},
            pair{"unsupportedOutput", LogMask::UnsupportedOutput},
            pair{"rawInput", LogMask::RawInput},
            pair{"rawOutput", LogMask::RawOutput},
            pair{"traceInput", LogMask::TraceInput},
            pair{"traceOutput", LogMask::TraceOutput},
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

std::string serializeYaml(Config const& _config)
{
    YAML::Node root;
    root["shell"] = _config.shell;
    for (auto const [key, value] : _config.env)
        root["environment"][key] = value;
    root["terminalSize"]["columns"] = _config.terminalSize.columns;
    root["terminalSize"]["lines"] = _config.terminalSize.rows;
    root["fontSize"] = _config.fontSize;
    root["fontFamily"] = _config.fontFamily;
    root["tabWidth"] = _config.tabWidth;
    root["background"]["opacity"] = static_cast<float>(_config.backgroundOpacity) / 255.0f;
    root["background"]["blur"] = _config.backgroundBlur;

    // history
    root["history"]["limit"] = _config.maxHistoryLineCount.has_value()
        ? static_cast<int64_t>(_config.maxHistoryLineCount.value())
        : -1ll;
    root["history"]["autoScrollOnUpdate"] = _config.autoScrollOnUpdate;
    root["history"]["scrollMultiplier"] = _config.historyScrollMultiplier;

    // colors
    root["colors"]["default"]["foreground"] = to_string(_config.colorProfile.defaultForeground);
    root["colors"]["default"]["background"] = to_string(_config.colorProfile.defaultBackground);
    root["colors"]["selection"] = to_string(_config.colorProfile.selection);

    constexpr auto names = array{"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
    for (size_t i = 0; i < names.size(); ++i)
    {
        root["colors"]["normal"][names[i]] = to_string(_config.colorProfile.normalColor(i));
        root["colors"]["bright"][names[i]] = to_string(_config.colorProfile.brightColor(i));
    }

    // cursor
    root["cursor"]["shape"] = to_string(_config.cursorShape);
    root["cursor"]["blinking"] = _config.cursorDisplay == terminal::CursorDisplay::Blink;

    // logging
    root["logging"]["parseErrors"] = (_config.loggingMask & LogMask::ParserError) != 0;
    root["logging"]["invalidOutput"] = (_config.loggingMask & LogMask::InvalidOutput) != 0;
    root["logging"]["unsupportedOutput"] = (_config.loggingMask & LogMask::UnsupportedOutput) != 0;
    root["logging"]["rawInput"] = (_config.loggingMask & LogMask::RawInput) != 0;
    root["logging"]["rawOutput"] = (_config.loggingMask & LogMask::RawOutput) != 0;
    root["logging"]["traceInput"] = (_config.loggingMask & LogMask::TraceInput) != 0;
    root["logging"]["traceOutput"] = (_config.loggingMask & LogMask::TraceOutput) != 0;

    // input mapping
    // TODO

    ostringstream os;
    os << root;// TODO: returns LF? if not, endl it.
    return os.str();
}

void saveConfigToFile(Config const& _config, FileSystem::path const& _path)
{
	FileSystemError ec;
	if (!FileSystem::create_directories(_path.parent_path(), ec))
	{
		throw runtime_error{fmt::format(
				"Could not create directory {}. {}",
				_path.parent_path().string(),
				ec.message())};
	}

    auto ofs = ofstream{_path.string(), ios::app};
    if (!ofs.good())
        throw runtime_error{ "Unable to create config file." };

     ofs << serializeYaml(_config);
}
