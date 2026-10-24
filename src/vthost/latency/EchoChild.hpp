// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The application a latency measurement types at: it prints a prompt, echoes what it reads, and ends on
/// QuitByte. Raw terminal I/O only; what it answers to each byte is echoReply(), which is tested.

#include <chrono>
#include <string_view>

namespace vthost::latency
{

/// Puts the terminal on this process's standard input and output into raw mode, prints the prompt, and
/// answers every byte it reads with echoReply() until QuitByte.
/// @return EXIT_SUCCESS once it read QuitByte or its input ended; EXIT_FAILURE if the terminal refused raw
///         mode.
[[nodiscard]] int runEchoChild();

/// Puts the terminal into raw mode and writes @p workload, over and over, for @p duration; then prints
/// FloodEndMarker and the byte count, and waits for QuitByte.
/// @param workload The name of a vtbackend::test::Workloads row.
/// @param duration How long to keep writing.
/// @return EXIT_SUCCESS, or EXIT_FAILURE for an unknown workload or a terminal that refused raw mode.
[[nodiscard]] int runFloodChild(std::string_view workload, std::chrono::seconds duration);

} // namespace vthost::latency
