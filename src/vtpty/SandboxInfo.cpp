// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SandboxInfo.hpp>

#include <crispy/Utils.hpp>

#include <filesystem>

namespace vtpty
{

namespace
{
    constexpr auto FlatpakInfoPath = std::string_view { "/.flatpak-info" };

    /// Both ends, because a key-file writes `shared = ipc;` as readily as `shared=ipc;`.
    ///
    /// crispy has trimRight() but no left or both-ends counterpart, and net/HttpServer.cpp carries
    /// its own copy of this for the same reason; a crispy::trim() would retire all three.
    [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
    {
        auto const isSpace = [](char ch) {
            return ch == ' ' || ch == '\t' || ch == '\r';
        };
        while (!text.empty() && isSpace(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && isSpace(text.back()))
            text.remove_suffix(1);
        return text;
    }

    /// Whether @p list, a semicolon-separated key-file string list, contains @p wanted.
    ///
    /// Compared whole rather than searched for as a substring, so a future share named
    /// "network-manager" could not be mistaken for "network".
    [[nodiscard]] bool listContains(std::string_view list, std::string_view wanted)
    {
        // split() stops as soon as the callback returns false, and reports that it did -- so "the
        // walk did not run to the end" IS "the item was found".
        return !crispy::split(list, ';', [wanted](std::string_view item) { return trimmed(item) != wanted; });
    }

} // namespace

SandboxInfo parseFlatpakInfo(std::string_view flatpakInfo)
{
    auto info =
        SandboxInfo { .state = SandboxState::Flatpak, .network = NetworkAccess::Denied, .applicationId = {} };

    auto group = std::string_view {};
    crispy::split(flatpakInfo, '\n', [&](std::string_view rawLine) {
        auto const line = trimmed(rawLine);

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

        auto const key = trimmed(line.substr(0, equals));
        auto const value = trimmed(line.substr(equals + 1));

        if (group == "Context" && key == "shared" && listContains(value, "network"))
            info.network = NetworkAccess::Permitted;
        else if (group == "Application" && key == "name")
            info.applicationId = std::string { value };

        return true; // every line is read; no line ends the walk early
    });

    return info;
}

SandboxInfo const& currentSandbox()
{
    // Memoized for the reason Process::isFlatpak() is: a process does not move between sandboxes, so
    // neither the file's existence nor its contents can change under us. A function-local static is
    // what makes that safe against the first two callers racing -- the SSH failure path runs on the
    // reader thread while socket resolution runs on the GUI thread.
    static SandboxInfo const info = []() -> SandboxInfo {
        // Guarded rather than caught: readFileAsString() asks for the size first, which throws when
        // there is no file -- and there being no file is the ordinary case, not an error.
        auto const path = std::filesystem::path { FlatpakInfoPath };
        if (!std::filesystem::is_regular_file(path))
            return SandboxInfo {}; // Host, network permitted, no application id.

        return parseFlatpakInfo(crispy::readFileAsString(path));
    }();

    return info;
}

} // namespace vtpty
