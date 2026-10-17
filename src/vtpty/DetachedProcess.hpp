// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef _WIN32

    #include <vtpty/ProcessPlacement.hpp>

    #include <cstdint>
    #include <expected>
    #include <filesystem>
    #include <string>
    #include <system_error>
    #include <vector>

namespace vtpty
{

/// Where a detached process's standard output and standard error go.
enum class DetachedOutput : std::uint8_t
{
    Inherited, ///< Wherever this process's own go.
    Discarded, ///< Nowhere: to /dev/null.
};

/// What startDetached() runs.
struct DetachedCommand
{
    /// The file to execute, as a path: nothing is searched for on PATH, which the child may not do.
    std::string image;

    /// The argument vector, the name the program runs under first; empty runs it under @ref image alone.
    std::vector<std::string> argv;

    /// The file its standard input reads; empty for this process's own standard input.
    std::filesystem::path standardInput;

    /// Where its standard output and standard error go.
    DetachedOutput output = DetachedOutput::Inherited;

    /// What the program is to Contour, as the placement's log and its scope tell.
    ProcessRole role = ProcessRole::Program;
};

/// Starts @p command detached, placed by @p placement before it runs -- as a session's shell is, so
/// that nothing it starts lives in this process's own resource domain.
///
/// Detached: in a session of its own, and not this process's child, so nothing here waits for it or
/// reaps it, and it outlives this process. It is parked before exec() and handed to @p placement,
/// which places and releases it on its own time: this returns once the handover is done, without
/// waiting for either. A program that cannot be executed -- not executable, or a bad interpreter -- is
/// therefore not reported here: it exits with status 127 once released, which nothing here sees, unlike
/// QProcess::startDetached(), which waits for the exec. A caller checks that the program is there first.
///
/// Implemented in Process_unix.cpp, beside Process::start(), whose async-signal-safe child helpers it
/// shares.
///
/// @param command   What to run.
/// @param placement Places the process before it runs, and releases it.
/// @return The process's id; or why it could not be handed to @p placement -- the input file could not
///         be opened, a fork failed, or the program's pid never arrived. In that last case a program
///         already forked may run regardless, unplaced, once its gate closes.
[[nodiscard]] std::expected<int, std::error_code> startDetached(DetachedCommand const& command,
                                                                ProcessPlacement& placement);

} // namespace vtpty

#endif
