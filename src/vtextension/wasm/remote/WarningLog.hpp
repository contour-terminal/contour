// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The log of extensions running, but not quite as they should, which both processes' code writes to.

#include <core/log/LogStore.hpp>

namespace vtextension::wasm::remote
{

/// Warns of extensions running, but not quite as they should: a sandbox short of a step, or a channel that
/// took in what no peer of it should send, is worth knowing about whether or not anyone debugs extensions, so
/// it is on unless turned off. At namespace scope, so that it is registered -- listed, and turned off by a
/// filter -- before anything logs.
extern core::log::Category const warningLog;

} // namespace vtextension::wasm::remote
