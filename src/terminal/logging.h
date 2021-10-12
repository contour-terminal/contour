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

#include <crispy/logstore.h>

namespace terminal {

auto const inline TerminalLog = logstore::Category("vt.session", "Logs general terminal events.");
auto const inline InputLog    = logstore::Category("vt.input", "Logs terminal keyboard/mouse input events.");
auto const inline VTParserLog = logstore::Category("vt.parser", "Logs terminal parser errors.", logstore::Category::State::Enabled, logstore::Category::Visibility::Hidden);

#if defined(LIBTERMINAL_LOG_RAW)
auto const inline ScreenRawOutputLog = logstore::Category("vt.output", "Logs raw writes to the terminal screen.");
#endif

#if defined(LIBTERMINAL_LOG_TRACE)
auto const inline VTParserTraceLog = logstore::Category("vt.trace", "Logs terminal parser instruction trace.");
#endif

auto const inline RenderBufferLog = logstore::Category("vt.renderbuffer", "Render Buffer Objects");

}
