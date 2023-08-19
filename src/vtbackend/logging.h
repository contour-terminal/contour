// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.h>

namespace terminal
{

auto const inline terminalLog = logstore::category("vt.session", "Logs general terminal events.");
auto const inline inputLog = logstore::category("vt.input", "Logs terminal keyboard/mouse input events.");
auto const inline vtParserLog = logstore::category("vt.parser",
                                                   "Logs terminal parser errors.",
                                                   logstore::category::state::Enabled,
                                                   logstore::category::visibility::Hidden);

#if defined(LIBTERMINAL_LOG_TRACE)
auto const inline vtTraceSequenceLog = logstore::category("vt.trace.sequence", "Logs terminal screen trace.");
#endif

auto const inline renderBufferLog = logstore::category("vt.renderbuffer", "Render Buffer Objects");

} // namespace terminal
