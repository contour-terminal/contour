// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/DaemonRelocation.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

#include <vthost/SocketPath.hpp>

namespace vthost::client
{

namespace
{
    /// @return @p path with forward slashes, where @p platform spells it with either, or the build's own file
    ///         system spells it with backslashes.
    [[nodiscard]] std::filesystem::path withForwardSlashes(std::filesystem::path const& path,
                                                           HostPlatform platform)
    {
        auto text = core::pathToUtf8(path);
        if (std::filesystem::path::preferred_separator == '\\' || platform == HostPlatform::Windows)
            std::ranges::replace(text, '\\', '/');
        return core::pathFromTrustedUtf8(text);
    }

    /// @return @p directory spelt so that two spellings of one directory compare equal on @p platform.
    [[nodiscard]] std::string comparableDirectory(std::filesystem::path const& directory,
                                                  HostPlatform platform)
    {
        // Separators first, so that `.` and `..` segments are normalized whichever separator spelt them.
        auto text = core::pathToUtf8(withForwardSlashes(directory, platform).lexically_normal());
        if (std::filesystem::path::preferred_separator == '\\')
            std::ranges::replace(text, '\\', '/');
        if (platform == HostPlatform::Windows)
        {
            std::ranges::transform(text, text.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
        }
        while (text.size() > 1 && text.ends_with('/'))
            text.pop_back();
        return text;
    }
} // namespace

RelocationVerdict relocationVerdictFor(DaemonStatus const& status,
                                       std::filesystem::path const& installationBin,
                                       HostPlatform platform)
{
    if (comparableDirectory(withForwardSlashes(status.executablePath, platform).parent_path(), platform)
        != comparableDirectory(installationBin, platform))
        return RelocationVerdict::OtherInstallation;
    if (!status.relocation)
        return RelocationVerdict::PredatesRelocation;
    if (*status.relocation == RelocationRequest::Refused)
        return RelocationVerdict::InPlaceRequested;
    return RelocationVerdict::Move;
}

std::vector<std::filesystem::path> controlSocketsAmong(std::span<std::filesystem::path const> entries)
{
    auto const present = std::set<std::filesystem::path> { entries.begin(), entries.end() };
    // A sibling is an entry that is another entry's path and a suffix.
    auto const isSibling = [&](std::filesystem::path const& entry) {
        auto const name = core::pathToUtf8(entry.filename());
        return std::ranges::any_of(SiblingSuffixes, [&](std::string_view suffix) {
            return name.size() > suffix.size() && name.ends_with(suffix)
                   && present.contains(
                       entry.parent_path()
                       / core::pathFromTrustedUtf8(name.substr(0, name.size() - suffix.size())));
        });
    };
    auto sockets = std::vector<std::filesystem::path> {};
    std::ranges::copy_if(
        entries, std::back_inserter(sockets), [&](auto const& entry) { return !isSibling(entry); });
    return sockets;
}

} // namespace vthost::client
