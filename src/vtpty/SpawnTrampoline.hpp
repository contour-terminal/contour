// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/SpawnSpec.hpp>

#ifndef _WIN32
    #include <vtpty/ExecutablePath.hpp>
    #include <vtpty/OwnedFd.hpp>
    #include <vtpty/ProcessPlacement.hpp>

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
/// @param argv The arguments main() was given; a `char* argv[]` converts.
/// @note Never returns from a trampoline invocation; otherwise returns at once.
void runSpawnTrampolineIfRequested(int argc, char const* const* argv) noexcept;

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

/// The environment the trampoline itself runs under, until its program's exec() installs the spec's.
enum class TrampolineEnvironment : std::uint8_t
{
    /// This process's, as it stands at the spawn: the dynamic loader finds Contour's libraries as it did
    /// for this process, whatever a session's configured environment does to the loader's variables.
    Inherited = 0,
    /// The spec's own, which must be given (SpawnSpec::environment): the spawn then reads nothing of this
    /// process's environment, which a caller off the thread that changes it must not.
    Program,
};

/// Spawns this process's own executable as a trampoline that starts @p spec's program.
///
/// posix_spawn() alone is used here, never fork(): safe to call from a multi-threaded process.
/// @param spec What to start; its inheritedSlots are filled from @p fds.
/// @param fds The descriptors to pass along, each at most once per slot; they stay open in this process.
/// @param environment What the trampoline runs under; Program requires a spec that carries an environment.
/// @return The trampoline's pid -- in Terminal mode the program's too, as its exec keeps it -- or why
///         spawning it or handing it the spec failed.
[[nodiscard]] std::expected<pid_t, std::error_code> spawnTrampoline(SpawnSpec spec,
                                                                    std::span<InheritedFd const> fds,
                                                                    TrampolineEnvironment environment);

/// spawnTrampoline() with an explicit trampoline image: the seam the tests use.
/// @param image The executable to spawn as the trampoline.
/// @param spec What to start; its inheritedSlots are filled from @p fds.
/// @param fds The descriptors to pass along, each at most once per slot; they stay open in this process.
/// @param environment What the trampoline runs under; Program requires a spec that carries an environment.
/// @return The spawned process's pid, or why spawning it or handing it the spec failed.
[[nodiscard]] std::expected<pid_t, std::error_code> spawnTrampolineImage(std::string const& image,
                                                                         SpawnSpec spec,
                                                                         std::span<InheritedFd const> fds,
                                                                         TrampolineEnvironment environment);

/// Spawns @p spec's program through the trampoline, parked on a gate of its own, and hands it to
/// @p placement, which places it and releases it to run.
///
/// In Detached mode the trampoline is waited for here and the program's pid read from the gate, as it is no
/// child of this process: briefly, the time a fork takes, on the calling thread.
/// @param spec What to start; its OOM score adjustment is @p placement's (childOomScoreAdjust()).
/// @param fds The descriptors to pass along besides the gate, which this function adds.
/// @param placement Places the program before it runs, and releases it.
/// @param role What the program is to Contour.
/// @param environment What the trampoline runs under.
/// @return The program's pid -- in Terminal mode a child of this process, in Detached mode not -- or why it
///         could not be spawned or handed to @p placement: protocol_not_supported or bad_message where the
///         trampoline refused its spec, no_child_process where a detached program's pid never arrived.
[[nodiscard]] std::expected<pid_t, std::error_code> spawnPlaced(SpawnSpec spec,
                                                                std::span<InheritedFd const> fds,
                                                                ProcessPlacement& placement,
                                                                ProcessRole role,
                                                                TrampolineEnvironment environment);

/// A program running on a terminal of its own, which this process holds the master of.
struct TerminalProgram
{
    pid_t pid;      ///< The program, a child of this process.
    OwnedFd master; ///< The terminal's master, close-on-exec.
};

/// Opens a new pseudo terminal and spawnPlaced()s @p spec's program onto it.
/// @param spec What to start; a TerminalLaunch.
/// @param placement Places the program before it runs, and releases it.
/// @param role What the program is to Contour.
/// @return The program and its terminal's master; or why it could not be started -- invalid_argument for a
///         spec of another mode.
[[nodiscard]] std::expected<TerminalProgram, std::error_code> spawnOnNewTerminal(SpawnSpec spec,
                                                                                 ProcessPlacement& placement,
                                                                                 ProcessRole role);

#endif

} // namespace vtpty
