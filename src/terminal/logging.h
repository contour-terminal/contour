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
#pragma once

#include <crispy/debuglog.h>

namespace terminal {

auto const inline TerminalTag = crispy::debugtag::make("vt.session", "Logs general terminal events.");
auto const inline InputTag    = crispy::debugtag::make("vt.input", "Logs terminal keyboard/mouse input events.");
auto const inline VTParserTag = crispy::debugtag::make("vt.parser", "Logs terminal parser errors.", true);

#if defined(LIBTERMINAL_LOG_RAW)
auto const inline ScreenRawOutputTag = crispy::debugtag::make("vt.output", "Logs raw writes to the terminal screen.");
#endif

#if defined(LIBTERMINAL_LOG_TRACE)
auto const inline VTParserTraceTag = crispy::debugtag::make("vt.trace", "Logs terminal parser instruction trace.");
#endif

}
