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
#include <glterminal/GLLogger.h>

#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <fmt/format.h>

#include <array>

using namespace std;
using namespace terminal;

GLLogger::GLLogger(LogMask _logMask, std::ostream* _sink) :
    logMask_{ _logMask },
    sink_{ _sink }
{
}

void GLLogger::keyPress(Key _key, Modifier _modifier)
{
    log(RawInputEvent{ fmt::format("key: {} {}", to_string(_key), to_string(_modifier)) });
}

void GLLogger::keyPress(char32_t _char, Modifier _modifier)
{
    if (utf8::isASCII(_char) && isprint(_char))
        log(RawInputEvent{ fmt::format("char: {} ({})", static_cast<char>(_char), to_string(_modifier)) });
    else
        log(RawInputEvent{ fmt::format("char: 0x{:04X} ({})", static_cast<uint32_t>(_char), to_string(_modifier)) });
}

void GLLogger::keyTrace(string const& _message)
{
    log(TraceInputEvent{_message});
}

void GLLogger::log(LogEvent const& _event)
{
    LogMask const m = visit(overloaded{
            [&](ParserErrorEvent const& v) {
                return LogMask::ParserError;
            },
            [&](RawInputEvent const& v) {
                return LogMask::RawInput;
            },
            [&](RawOutputEvent const& v) {
                return LogMask::RawOutput;
            },
            [&](InvalidOutputEvent const& v) {
                return LogMask::InvalidOutput;
            },
            [&](UnsupportedOutputEvent const& v) {
                return LogMask::UnsupportedOutput;
            },
            [&](TraceInputEvent const& v) {
                return LogMask::TraceInput;
            },
            [&](TraceOutputEvent const& v) {
                return LogMask::TraceOutput;
            },
        },
        _event
    );

    if ((logMask_ & m) != LogMask::None)
        *sink_ << fmt::format("{}\n", _event);
}

void GLLogger::flush()
{
    if (sink_)
        sink_->flush();
}
