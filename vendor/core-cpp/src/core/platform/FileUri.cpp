// SPDX-License-Identifier: Apache-2.0
#include <core/platform/FileUri.hpp>

#include <array>

namespace core::platform
{

namespace
{
    /// @brief Bytes that need no percent-encoding under RFC 3986.
    ///
    /// The unreserved set, spelled once as data. Built at compile time so classification is a
    /// single indexed load and, unlike a ctype call, cannot vary with locale.
    constexpr auto Unreserved = [] {
        auto table = std::array<bool, 256> {};
        for (auto const ch:
             std::string_view { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~" })
            table[static_cast<unsigned char>(ch)] = true;
        return table;
    }();

    constexpr auto PathSeparator = std::string_view { "/" };

    constexpr auto HexDigits = std::string_view { "0123456789ABCDEF" };

    constexpr auto SchemePrefix = std::string_view { "file://" };

    /// @brief Whether @p path starts with a Windows drive specifier such as `C:`.
    [[nodiscard]] constexpr bool hasDriveSpecifier(std::string_view path) noexcept
    {
        if (path.size() < 2 || path[1] != ':')
            return false;
        auto const drive = path[0];
        return (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
    }
} // namespace

void appendPercentEncoded(std::string& out, std::string_view input, std::string_view extraSafe)
{
    for (auto const ch: input)
    {
        auto const byte = static_cast<unsigned char>(ch);
        if (Unreserved[byte] || extraSafe.contains(ch))
        {
            out += ch;
        }
        else
        {
            out += '%';
            out += HexDigits[byte >> 4U];
            out += HexDigits[byte & 0x0FU];
        }
    }
}

auto percentEncode(std::string_view input, std::string_view extraSafe) -> std::string
{
    auto encoded = std::string {};
    encoded.reserve(input.size());
    appendPercentEncoded(encoded, input, extraSafe);
    return encoded;
}

void appendPercentEncodedUriPath(std::string& out, std::string_view path)
{
    appendPercentEncoded(out, path, PathSeparator);
}

auto percentEncodeUriPath(std::string_view path) -> std::string
{
    return percentEncode(path, PathSeparator);
}

auto fileUri(std::string_view absolutePath, std::string_view host, std::string_view fragment) -> std::string
{
    if (absolutePath.empty())
        return {};

    auto authority = host;
    auto path = absolutePath;

    if (path.starts_with("//"))
    {
        // UNC path: `//server/share/x`. RFC 8089 puts the server in the authority, so the
        // local hostname is irrelevant here — the file does not live on this machine.
        path.remove_prefix(2);
        auto const slash = path.find('/');
        authority = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view {} : path.substr(slash);
    }

    auto uri = std::string {};
    // Worst case every path byte triples; the common case is no encoding at all, so reserve for
    // the plain length and let the rare escape grow it.
    uri.reserve(SchemePrefix.size() + authority.size() + path.size() + fragment.size() + 4);
    uri += SchemePrefix;
    uri += authority;

    if (hasDriveSpecifier(path))
    {
        // A drive-letter path has no leading slash of its own, but the URI scheme requires
        // the path component to start with one: `C:/x` -> `/C:/x`. The drive specifier is
        // emitted verbatim so the colon survives — it is a valid pchar and is the form
        // terminals and `wslpath` expect. Colons elsewhere are still encoded.
        uri += '/';
        uri += path.substr(0, 2);
        path.remove_prefix(2);
    }

    appendPercentEncodedUriPath(uri, path);

    if (!fragment.empty())
    {
        uri += '#';
        appendPercentEncodedUriPath(uri, fragment);
    }

    return uri;
}

} // namespace core::platform
