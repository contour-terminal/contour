// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The thread pools Contour runs blocking background work on, made so that no signal ever lands on them.

#include <core/async/ThreadPoolExecutor.hpp>

#include <cstddef>

namespace contour::cli
{

/// Creates a pool for background work: status-line commands, link matching, path probes, a live upgrade's
/// readiness probe.
///
/// On POSIX the workers are created with every asynchronous signal blocked. Otherwise they would keep SIGINT
/// and SIGTERM unblocked, and the kernel may deliver a process-directed signal to any thread that does not
/// block it: `contour daemon` blocks them in its own thread and consumes them on a dedicated sigwait thread
/// (vthost::runDaemon), so a delivery to a pool worker would kill the process with the default disposition
/// before the daemon could clean up its socket. The spawned shells do not inherit the blocked mask
/// (SystemCommandCapture resets it).
/// @param threads The number of workers.
/// @return The pool.
[[nodiscard]] core::async::ThreadPoolExecutor makeBackgroundExecutor(std::size_t threads);

} // namespace contour::cli
