// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SandboxInfo.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <filesystem>
#include <utility>

namespace vtpty
{

namespace
{
    constexpr auto FlatpakInfoPath = std::string_view { "/.flatpak-info" };

    /// Whether @p list, a semicolon-separated key-file string list, contains @p wanted.
    ///
    /// Trimmed at both ends, because a key-file writes `shared = ipc ; network` as readily as
    /// `shared=ipc;network`. Compared whole rather than searched for as a substring, so a future
    /// share named "network-manager" could not be mistaken for "network".
    [[nodiscard]] bool listContains(std::string_view list, std::string_view wanted)
    {
        // split() stops as soon as the callback returns false, and reports that it did -- so "the
        // walk did not run to the end" IS "the item was found".
        return !core::split(
            list, ';', [wanted](std::string_view item) { return core::trim(item) != wanted; });
    }

} // namespace

SandboxInfo parseFlatpakInfo(std::string_view flatpakInfo)
{
    auto info =
        SandboxInfo { .state = SandboxState::Flatpak, .network = NetworkAccess::Denied, .applicationId = {} };

    auto group = std::string_view {};
    core::split(flatpakInfo, '\n', [&](std::string_view rawLine) {
        auto const line = core::trim(rawLine);

        if (line.empty() || line.front() == '#')
            return true;

        if (line.front() == '[' && line.back() == ']')
        {
            group = line.substr(1, line.size() - 2);
            return true;
        }

        auto const equals = line.find('=');
        if (equals == std::string_view::npos)
            return true;

        auto const key = core::trim(line.substr(0, equals));
        auto const value = core::trim(line.substr(equals + 1));

        if (group == "Context" && key == "shared" && listContains(value, "network"))
            info.network = NetworkAccess::Permitted;
        else if (group == "Application" && key == "name")
            info.applicationId = std::string { value };

        return true; // every line is read; no line ends the walk early
    });

    return info;
}

std::optional<std::filesystem::path> sharedRuntimeDirectory(std::optional<std::string_view> xdgRuntimeDir,
                                                            std::string_view applicationId)
{
    if (!xdgRuntimeDir || xdgRuntimeDir->empty())
        return std::nullopt;

    auto directory = core::pathFromUtf8(*xdgRuntimeDir);
    if (!directory || !directory->has_root_directory())
        return std::nullopt;

    if (applicationId.empty())
        return std::move(*directory);

    // Inside the sandbox the runtime directory itself is private: only the application's own
    // subdirectory is shared, and there is nothing to fall back to.
    auto const application = core::pathFromUtf8(applicationId);
    if (!application)
        return std::nullopt;
    return *directory / "app" / *application;
}

SandboxInfo const& currentSandbox()
{
    // A process does not move between sandboxes, so neither the file's existence nor its contents
    // can change under us. A function-local static is what makes that safe against the first two
    // callers racing -- the SSH failure path runs on the reader thread while socket resolution runs
    // on the GUI thread.
    static SandboxInfo const info = []() -> SandboxInfo {
        // Guarded rather than caught: readFileAsString() asks for the size first, which throws when
        // there is no file -- and there being no file is the ordinary case, not an error.
        auto const path = std::filesystem::path { FlatpakInfoPath };
        if (!std::filesystem::is_regular_file(path))
            return SandboxInfo {}; // Host, network permitted, no application id.

        return parseFlatpakInfo(core::readFileAsString(path));
    }();

    return info;
}

} // namespace vtpty
