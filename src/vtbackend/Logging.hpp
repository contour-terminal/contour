// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/LogStore.hpp>

namespace vtbackend
{

auto inline const terminalLog = logstore::Category("vt.session", "Logs general terminal events.");
auto inline const inputLog = logstore::Category("vt.input", "Logs terminal keyboard/mouse input events.");
auto inline const vtParserLog = logstore::Category("vt.parser",
                                                   "Logs terminal parser errors.",
                                                   logstore::Category::State::Enabled,
                                                   logstore::Category::Visibility::Hidden);

#ifdef LIBTERMINAL_LOG_TRACE
auto inline const vtTraceSequenceLog = logstore::Category("vt.trace.sequence", "Logs terminal screen trace.");
#endif

auto inline const renderBufferLog = logstore::Category("vt.renderbuffer", "Render Buffer Objects");

} // namespace vtbackend
