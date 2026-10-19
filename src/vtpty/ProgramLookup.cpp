// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProgramLookup.hpp>

#include <core/Utf8Path.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
    #include <paths.h>
    #include <unistd.h>
#endif

namespace vtpty
{

namespace
{
    /// Finds the file a candidate path names to run, if any.
    using FileFinder = std::function<std::optional<std::filesystem::path>(std::filesystem::path const&)>;

    /// @return The file @p find finds for @p named in the first entry of @p searchPath that has one; an
    ///         empty entry, which POSIX reads as the current directory, deliberately skipped, because
    ///         resolving a name against the cwd is the ambiguity a PATH walk exists to remove.
    [[nodiscard]] std::optional<std::string> findOnPath(std::filesystem::path const& named,
                                                        std::string_view searchPath,
                                                        FileFinder const& find)
    {
        for (auto const entry: std::views::split(searchPath, PathListSeparator))
        {
            auto const directory = core::pathFromUtf8(std::string_view { entry });
            if (!directory || directory->empty())
                continue;
            if (auto const found = find(*directory / named))
                return core::pathToUtf8(*found);
        }
        return std::nullopt;
    }
} // namespace

std::string resolveExecutablePath(std::string_view program,
                                  std::string_view searchPath,
                                  core::platform::FileSystem const& fileSystem)
{
    auto const named = core::pathFromUtf8(program);
    // A name carrying a directory is already a path — execvp's rule, and what keeps `./contour`
    // and `/usr/bin/contour` meaning exactly what they say rather than being searched for.
    if (program.empty() || !named || named->has_parent_path())
        return std::string { program };

    auto const executable = [&fileSystem](std::filesystem::path const& candidate) {
        return fileSystem.isExecutableFile(candidate) ? std::optional { candidate } : std::nullopt;
    };
    return findOnPath(*named, searchPath, executable).value_or(std::string { program });
}

std::optional<std::string> findProgram(std::string_view program, ProgramSearch const& search)
{
    auto const programFile = [&fileSystem = search.fileSystem](
                                 std::filesystem::path candidate) -> std::optional<std::filesystem::path> {
#ifdef _WIN32
        // CreateProcess runs "pwsh" as "pwsh.exe", and a name without an extension only so: a file without
        // one -- a stray "C:\Program" -- it never runs.
        if (!candidate.has_extension())
            candidate += L".exe";
#endif
        if (!fileSystem.isExecutableFile(candidate))
            return std::nullopt;
        return candidate;
    };

    auto const named = core::pathFromUtf8(program);
    if (program.empty() || !named)
        return std::nullopt;
    if (named->has_parent_path())
        return programFile(named->is_relative() ? search.workingDirectory / *named : *named)
            .transform([](std::filesystem::path const& found) { return core::pathToUtf8(found); });
    return findOnPath(*named, search.path, programFile);
}

std::string execSearchPath(std::optional<std::string> environmentPath)
{
    if (environmentPath)
        return std::move(*environmentPath);
    // Each C library's execvp() searches a default of its own where PATH is unset.
#ifdef __GLIBC__
    // glibc's execvpe() searches CS_PATH, which confstr(_CS_PATH) answers -- not <paths.h>'s _PATH_DEFPATH.
    // The size includes the terminating null, which confstr() writes too.
    auto const size = ::confstr(_CS_PATH, nullptr, 0);
    if (size == 0)
        return {};
    auto path = std::string(size, '\0');
    (void) ::confstr(_CS_PATH, path.data(), path.size());
    path.pop_back();
    return path;
#elifdef _PATH_DEFPATH
    // FreeBSD's, Darwin's and musl's execvp() search <paths.h>'s _PATH_DEFPATH; their confstr(_CS_PATH)
    // answers another path.
    return _PATH_DEFPATH;
#else
    return {};
#endif
}

bool namesProgram(std::string_view program, ProgramSearch const& search)
{
    return findProgram(program, search).has_value();
}

ProgramPredicate programsIn(ProgramSearch const& search)
{
    return [search](std::string_view program) {
        return namesProgram(program, search);
    };
}

ProgramPredicate programsFor(ProgramHost host, ProgramSearch const& search)
{
    switch (host)
    {
        case ProgramHost::ThisMachine: return programsIn(search);
        case ProgramHost::SandboxHost:
            return [](std::string_view) {
                return false;
            };
    }
    return programsIn(search);
}

} // namespace vtpty
