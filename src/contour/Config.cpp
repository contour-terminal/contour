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
#include "Flags.h"
#include <terminal/Process.h>
#include <yaml-cpp/yaml.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <glterminal/GLCursor.h>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace std;

optional<int> loadConfigFromCLI(Config& _config, int _argc, char const* _argv[])
{
    util::Flags flags;
    flags.defineBool("help", 'h', "Shows this help and quits.");
    flags.defineBool("version", 'v', "Shows this version and exits.");
    flags.defineString("config", 'c', "PATH", "Specifies path to config file to load from (and save to).", "contour.yml");

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

void loadConfigFromFile(Config& _config, std::string const& _fileName)
{
    YAML::Node doc = YAML::LoadFile(_fileName);

    _config.backingFilePath = FileSystem::path{_fileName};

    softLoadValue(doc, "shell", _config.shell);

    if (auto terminalSize = doc["terminalSize"]; terminalSize)
    {
        softLoadValue(terminalSize, "columns", _config.terminalSize.columns);
        softLoadValue(terminalSize, "lines", _config.terminalSize.rows);
    }

    softLoadValue(doc, "fontSize", _config.fontSize);
    softLoadValue(doc, "fontFamily", _config.fontFamily);
    softLoadValue(doc, "tabWidth", _config.tabWidth);

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
            _config.cursorShape = makeCursorShape(shape.as<string>());

        softLoadValue(cursor, "blinking", _config.cursorBlinking);
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
    root["terminalSize"]["columns"] = _config.terminalSize.columns;
    root["terminalSize"]["lines"] = _config.terminalSize.rows;
    root["fontSize"] = _config.fontSize;
    root["fontFamily"] = _config.fontFamily;
    root["tabWidth"] = _config.tabWidth;
    root["background"]["opacity"] = static_cast<float>(_config.backgroundOpacity) / 255.0f;
    root["background"]["blur"] = _config.backgroundBlur;

    // TODO: colors

    root["cursor"]["shape"] = to_string(_config.cursorShape);
    root["cursor"]["blinking"] = _config.cursorBlinking;

    root["logging"]["parseErrors"] = (_config.loggingMask & LogMask::ParserError) != 0;
    root["logging"]["invalidOutput"] = (_config.loggingMask & LogMask::InvalidOutput) != 0;
    root["logging"]["unsupportedOutput"] = (_config.loggingMask & LogMask::UnsupportedOutput) != 0;
    root["logging"]["rawInput"] = (_config.loggingMask & LogMask::RawInput) != 0;
    root["logging"]["rawOutput"] = (_config.loggingMask & LogMask::RawOutput) != 0;
    root["logging"]["traceInput"] = (_config.loggingMask & LogMask::TraceInput) != 0;
    root["logging"]["traceOutput"] = (_config.loggingMask & LogMask::TraceOutput) != 0;

    ostringstream os;
    os << root;// TODO: returns LF? if not, endl it.
    return os.str();
}

void saveConfigToFile(Config const& _config, std::string const& _fileName)
{
    auto ofs = ofstream{_fileName, ios::trunc};
    ofs << serializeYaml(_config);
}
