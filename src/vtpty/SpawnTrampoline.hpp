// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/SpawnSpec.hpp>

#ifndef _WIN32
    #include <sys/types.h>

    #include <cstdint>
    #include <expected>
    #include <span>
    #include <string>
    #include <system_error>
#endif

namespace vtpty
{

/// Runs this process as a spawn trampoline where its command line asks for one: the very first statement of
/// every main() that can reach a spawn site, before any other initialisation.
///
/// A trampoline reads a SpawnSpec from SpawnSpecFd, does the child-side setup a forked child did -- parking
/// on its placement gate, taking its terminal, installing its environment -- and execs the program. Being
/// single-threaded, it may do all of that freely, where a child forked from a multi-threaded process may
/// call only async-signal-safe functions. Does nothing on Windows.
/// @param argc The argument count main() was given.
/// @param argv The arguments main() was given.
/// @note Never returns from a trampoline invocation; otherwise returns at once.
void runSpawnTrampolineIfRequested(int argc, char const* const* argv) noexcept;

/// The same, for a main() declared with `char* argv[]`.
/// @param argc The argument count main() was given.
/// @param argv The arguments main() was given.
void runSpawnTrampolineIfRequested(int argc, char* const* argv) noexcept;

#ifndef _WIN32

/// A descriptor handed to the trampoline, and the slot it takes there.
struct InheritedFd
{
    TrampolineSlot slot; ///< Where it lands: trampolineFd(slot).
    int fd;              ///< The descriptor in this process; it stays open here.
};

/// Exit statuses the trampoline itself uses; the program's own statuses pass through.
enum class TrampolineExit : std::uint8_t
{
    SpecUnreadable = 120,     ///< SpawnSpecFd held no decodable spec.
    UnsupportedVersion = 121, ///< The marker named a protocol version this executable does not speak.
};

/// @return The file this process runs: "/proc/self/exe" on Linux, which names the running image even once a
///         package upgrade replaced it on disk; elsewhere the absolute path the platform reports.
[[nodiscard]] std::expected<std::string, std::error_code> selfExecutablePath();

/// Spawns this process's own executable as a trampoline that starts @p spec's program.
///
/// posix_spawn() alone is used here, never fork(): safe to call from a multi-threaded process.
/// @param spec What to start; its inheritedSlots are filled from @p fds.
/// @param fds The descriptors to pass along, each at most once per slot; they stay open in this process.
/// @return The trampoline's pid -- in Terminal mode the program's too, as its exec keeps it -- or why
///         spawning it or handing it the spec failed.
[[nodiscard]] std::expected<pid_t, std::error_code> spawnTrampoline(SpawnSpec spec,
                                                                    std::span<InheritedFd const> fds);

/// spawnTrampoline() with an explicit trampoline image: the seam the tests use.
/// @param image The executable to spawn as the trampoline.
/// @param spec What to start; its inheritedSlots are filled from @p fds.
/// @param fds The descriptors to pass along, each at most once per slot; they stay open in this process.
/// @return The spawned process's pid, or why spawning it or handing it the spec failed.
[[nodiscard]] std::expected<pid_t, std::error_code> spawnTrampolineImage(std::string const& image,
                                                                         SpawnSpec spec,
                                                                         std::span<InheritedFd const> fds);

#endif

} // namespace vtpty
