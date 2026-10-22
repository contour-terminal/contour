// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionGrantStore.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <utility>

namespace vtextension
{

namespace
{
    /// How one answer is spelled in a grants file.
    struct GrantSpelling
    {
        GrantState state;
        std::string_view word;
    };

    /// Every answer a file may hold. Pending is no answer, and is never written.
    constexpr auto GrantSpellings = std::array {
        GrantSpelling { .state = GrantState::Granted, .word = "granted" },
        GrantSpelling { .state = GrantState::Denied, .word = "denied" },
    };

    [[nodiscard]] std::optional<GrantState> grantStateOf(std::string_view word) noexcept
    {
        auto const spelling = std::ranges::find(GrantSpellings, word, &GrantSpelling::word);
        if (spelling == GrantSpellings.end())
            return std::nullopt;
        return spelling->state;
    }

    [[nodiscard]] std::optional<std::string_view> wordOf(GrantState state) noexcept
    {
        auto const spelling = std::ranges::find(GrantSpellings, state, &GrantSpelling::state);
        if (spelling == GrantSpellings.end())
            return std::nullopt;
        return spelling->word;
    }

    /// @return @p line without the CR a CRLF line end leaves behind.
    [[nodiscard]] std::string_view withoutCarriageReturn(std::string_view line) noexcept
    {
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        return line;
    }

    [[nodiscard]] bool isComment(std::string_view line) noexcept
    {
        return line.empty() || line.starts_with('#');
    }

    /// Reads one answer line: `<state> TAB <digest> TAB <host id>`.
    [[nodiscard]] std::optional<std::pair<ExtensionGrantKey, GrantState>> parseGrantLine(
        std::string_view line)
    {
        auto const firstTab = line.find('\t');
        if (firstTab == std::string_view::npos)
            return std::nullopt;
        auto const secondTab = line.find('\t', firstTab + 1);
        if (secondTab == std::string_view::npos)
            return std::nullopt;
        auto const state = grantStateOf(line.substr(0, firstTab));
        auto const digest = line.substr(firstTab + 1, secondTab - firstTab - 1);
        auto const hostId = line.substr(secondTab + 1);
        if (!state || digest.empty() || hostId.empty())
            return std::nullopt;
        return std::pair {
            ExtensionGrantKey { .hostId = std::string { hostId }, .digest = std::string { digest } }, *state
        };
    }

    [[nodiscard]] bool fitsOnALine(ExtensionGrantKey const& key) noexcept
    {
        auto const breaksLine = [](char c) {
            return c == '\n' || c == '\r';
        };
        return !key.hostId.empty() && !key.digest.empty() && std::ranges::none_of(key.hostId, breaksLine)
               && std::ranges::none_of(key.digest, breaksLine) && !key.digest.contains('\t');
    }
} // namespace

std::expected<ExtensionGrants, GrantFileError> parseExtensionGrants(std::string_view text)
{
    auto lines = text | std::views::split('\n') | std::views::transform([](auto const& range) {
                     return withoutCarriageReturn(std::string_view { range.begin(), range.end() });
                 });
    auto line = std::ranges::find_if_not(lines, isComment);
    if (line == lines.end() || *line != ExtensionGrantsHeader)
        return std::unexpected(GrantFileError::UnknownFormat);

    auto grants = ExtensionGrants {};
    for (auto const answer: std::ranges::subrange(std::ranges::next(line), lines.end()))
        if (!isComment(answer))
            if (auto parsed = parseGrantLine(answer))
                grants.insert_or_assign(std::move(parsed->first), parsed->second);
    return grants;
}

std::string serializeExtensionGrants(ExtensionGrants const& grants)
{
    auto text = std::string {};
    text += "# Contour: whether status-line extensions may run the commands their manifests declare.\n";
    text +=
        "# One answer per line: granted|denied TAB manifest digest TAB host id. Delete a line to be asked "
        "again.\n";
    text += ExtensionGrantsHeader;
    text += '\n';
    for (auto const& [key, state]: grants)
        if (auto const word = wordOf(state); word && fitsOnALine(key))
            text += std::format("{}\t{}\t{}\n", *word, key.digest, key.hostId);
    return text;
}

std::string daemonExtensionGrantsFileName(DaemonIdentity const& daemon)
{
    constexpr auto Suffix = std::string_view { ".txt" };
    static_assert(DaemonExtensionGrantsFileName.ends_with(Suffix));
    auto name = std::string { DaemonExtensionGrantsFileName.substr(
        0, DaemonExtensionGrantsFileName.size() - Suffix.size()) };
    if (!daemon.label.empty() && daemon.label != daemon.defaultLabel)
    {
        auto const keep = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'
                   || c == '-' || c == '_';
        };
        auto label = std::string { daemon.label };
        std::ranges::replace_if(label, [&](char c) { return !keep(c); }, '_');
        name += std::format(".{}", label);
    }
    if (auto const socket = daemon.socketPath.lexically_normal();
        socket != daemon.labelSocketPath.lexically_normal())
    {
        // FNV-1a over the path's UTF-8: stable across runs and builds, unlike std::hash, and short. A
        // collision costs two daemons one file, as a shared label always did.
        constexpr auto Offset = uint64_t { 14695981039346656037U };
        constexpr auto Prime = uint64_t { 1099511628211U };
        auto hash = Offset;
        for (auto const c: core::pathToUtf8(socket))
            hash = (hash ^ static_cast<uint8_t>(c)) * Prime;
        name += std::format(".{:08x}", static_cast<uint32_t>(hash ^ (hash >> 32U)));
    }
    return name.append(Suffix);
}

ExtensionGrantStore::ExtensionGrantStore(core::platform::FileSystem const& fileSystem,
                                         std::filesystem::path file):
    _fileSystem { fileSystem }, _file { std::move(file) }
{
    if (!_fileSystem.exists(_file))
        return;
    auto const text = _fileSystem.readFile(_file);
    if (!text)
    {
        errorLog()("Cannot read the extension grants {}: {}", core::pathToUtf8(_file), text.error());
        return;
    }
    if (auto grants = parseExtensionGrants(*text))
        _grants = std::move(*grants);
    else
        errorLog()("Ignoring the extension grants {}: not a grants file of this version; the next answer "
                   "replaces it.",
                   core::pathToUtf8(_file));
}

GrantState ExtensionGrantStore::lookup(std::string_view hostId, std::string_view digest) const
{
    auto const key = ExtensionGrantKey { .hostId = std::string { hostId }, .digest = std::string { digest } };
    auto const lock = std::scoped_lock { _mutex };
    auto const found = _grants.find(key);
    return found != _grants.end() ? found->second : GrantState::Pending;
}

void ExtensionGrantStore::record(std::string_view hostId, std::string_view digest, GrantState state)
{
    auto key = ExtensionGrantKey { .hostId = std::string { hostId }, .digest = std::string { digest } };
    auto const text = [&] {
        auto const lock = std::scoped_lock { _mutex };
        if (state == GrantState::Pending)
            _grants.erase(key);
        else
            _grants.insert_or_assign(std::move(key), state);
        return serializeExtensionGrants(_grants);
    }();

    // Written outside the lock, so the workers looking answers up never wait for the disk. Only the GUI
    // thread records, so two writes never race.
    auto const temporary = std::filesystem::path { _file.string() + ".tmp" };
    auto const written =
        _fileSystem.writeFile(temporary, text).and_then([&] { return _fileSystem.rename(temporary, _file); });
    if (!written)
        errorLog()("Cannot write the extension grants {}: {}; the answer holds until Contour exits.",
                   core::pathToUtf8(_file),
                   written.error());
}

} // namespace vtextension
