// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef _WIN32
    #include <vtpty/OwnedFd.hpp>
    #include <vtpty/Process.hpp>
    #include <vtpty/Pty.hpp>
    #include <vtpty/SandboxInfo.hpp>

    #include <expected>
    #include <optional>
    #include <string>
    #include <system_error>
    #include <utility>
    #include <vector>
#endif

namespace vtpty
{

#ifndef _WIN32

/// A spawn of the trampoline, prepared in full by this process and started by whoever forks it: this process
/// (Process::start()), or the PTY keeper, which is handed the argv and the descriptor table and only forks
/// and execs. Everything the trampoline is to know -- its spec, slot numbers, protocol version -- is decided
/// here, so the forking side depends on none of it.
struct PreparedSpawn
{
    /// The trampoline's argv: its name and the marker with the protocol version (SpawnTrampolineMarker).
    std::vector<std::string> argv;
    /// The child's descriptor table: child descriptor number <- a close-on-exec copy in this process.
    std::vector<std::pair<int, OwnedFd>> childFds;
    /// This process's end of the spec socket. The spec is written into it once the child exists
    /// (writeSpawnSpec()): a spec larger than the socket's buffer is read as it is written.
    OwnedFd specSocket;
    /// The spec, framed, as writeSpawnSpec() writes it.
    std::string spec;
    /// The parent's end of the placement gate; none for a spawn without one.
    OwnedFd gate;
    /// The parent's end of the settle signal (@see Process::spawnProgress()); none for a spawn without one.
    OwnedFd settle;
};

/// The parent-side half of starting a shell on @p startedPty, without the fork: the spec of the program to
/// run, the settle signal and the placement gate, and the trampoline's descriptor table. Always carries the
/// program's environment (SpawnSpec::environment), so that whoever forks the trampoline lends it nothing of
/// its own: under a sandbox escape the environment as this process has it, which flatpak-spawn passes on.
/// @param startedPty The terminal, started; its spawnDescriptors() are what the program gets.
/// @param exec What to run.
/// @param escape Whether the program escapes the sandbox this process runs in.
/// @param childOomScoreAdjust The oom_score_adj the program sets for itself, if any.
/// @return The prepared spawn; or why it could not be prepared.
[[nodiscard]] std::expected<PreparedSpawn, StartFailure> prepareTrampolineSpawn(
    Pty& startedPty,
    Process::ExecInfo const& exec,
    SandboxEscape escape,
    std::optional<int> childOomScoreAdjust);

/// Writes @p prepared's spec into its spec socket and closes the socket; called once the trampoline exists.
/// @param prepared A spawn whose trampoline was started.
/// @return Nothing once written; or why it could not be (the trampoline then reads end-of-file and exits).
[[nodiscard]] std::expected<void, std::error_code> writeSpawnSpec(PreparedSpawn& prepared);

/// Reads, without waiting, whether the child behind a settle signal (PreparedSpawn::settle) has settled:
/// readable means it has, by the child's byte or by end-of-file as it exited before.
/// @param settle The parent's end of the signal; closed once it reads settled, so that it is asked no more.
/// @return Settled for a closed @p settle or a readable one; InProgress otherwise.
[[nodiscard]] SpawnProgress settleProgressOf(OwnedFd& settle) noexcept;

#endif

} // namespace vtpty
