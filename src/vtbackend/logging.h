// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.h>

namespace vtbackend
{

auto inline const terminalLog = logstore::category("vt.session", "Logs general terminal events.");
auto inline const inputLog = logstore::category("vt.input", "Logs terminal keyboard/mouse input events.");
auto inline const vtParserLog = logstore::category("vt.parser",
                                                   "Logs terminal parser errors.",
                                                   logstore::category::state::Enabled,
                                                   logstore::category::visibility::Hidden);

#ifdef LIBTERMINAL_LOG_TRACE
auto inline const vtTraceSequenceLog = logstore::category("vt.trace.sequence", "Logs terminal screen trace.");
#endif

auto inline const renderBufferLog = logstore::category("vt.renderbuffer", "Render Buffer Objects");

} // namespace vtbackend
