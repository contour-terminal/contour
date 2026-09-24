// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/log/LogStore.hpp>

namespace vtbackend
{

auto inline const terminalLog = core::log::Category("vt.session", "Logs general terminal events.");
auto inline const inputLog = core::log::Category("vt.input", "Logs terminal keyboard/mouse input events.");
auto inline const vtParserLog = core::log::Category("vt.parser",
                                                    "Logs terminal parser errors.",
                                                    core::log::Category::State::Enabled,
                                                    core::log::Category::Visibility::Hidden);

#ifdef LIBTERMINAL_LOG_TRACE
auto inline const vtTraceSequenceLog =
    core::log::Category("vt.trace.sequence", "Logs terminal screen trace.");
#endif

auto inline const renderBufferLog = core::log::Category("vt.renderbuffer", "Render Buffer Objects");

} // namespace vtbackend
