// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/FileUrl.hpp>

#include <core/Utils.hpp>

namespace vtbackend
{

namespace
{
    constexpr auto Prefix = std::string_view("file://");

    /// Whether @p path opens with a Windows drive letter, e.g. "C:/Users". Such a prefix is a path, not a
    /// host, so a file://C:/path authority must not be read as one.
    [[nodiscard]] constexpr bool isDriveLetterPath(std::string_view path) noexcept
    {
        return path.size() >= 2 && (crispy::ascii::isLower(path[0]) || crispy::ascii::isUpper(path[0]))
               && path[1] == ':';
    }

    /// Whether @p path can name a file on a machine following @p convention.
    [[nodiscard]] constexpr bool namesAPathUnder(std::string_view path, PathConvention convention) noexcept
    {
        switch (convention)
        {
            case PathConvention::Windows: return true;
            // Not even an absolute path: opened, it would be some other file than the one the URL names.
            case PathConvention::Posix: return !isDriveLetterPath(path);
        }
        return false;
    }

    /// The authority of a file:// URL's remainder (what follows "file://"): everything up to the first
    /// '/'. Empty for the rooted file:///path form and for a Windows drive-letter path.
    [[nodiscard]] constexpr std::string_view authorityOf(std::string_view remainder) noexcept
    {
        if (remainder.empty() || remainder.front() == '/' || isDriveLetterPath(remainder))
            return {};
        return remainder.substr(0, remainder.find('/'));
    }

    /// The path of a file:// URL's remainder (what follows "file://"), still percent-encoded.
    [[nodiscard]] constexpr std::string_view encodedPathOf(std::string_view remainder) noexcept
    {
        // file:///path -> /path  ;  file://host/path -> /path  ;  file://C:/path -> C:/path
        if (!remainder.empty() && remainder[0] != '/')
        {
            if (isDriveLetterPath(remainder))
                return remainder;
            if (auto const pos = remainder.find('/'); pos != std::string_view::npos)
            {
                // file://host/C:/path -> C:/path : strip the leading slash before a Windows drive
                // letter so a host-qualified URL still yields a valid native absolute path.
                auto const pathPart = remainder.substr(pos);
                if (pathPart.size() >= 3 && isDriveLetterPath(pathPart.substr(1)))
                    return pathPart.substr(1);
                return pathPart;
            }
            return {};
        }

        // file:///C:/path -> C:/path : strip the leading slash before a Windows drive letter so the
        // resulting string is a valid native absolute path rather than a rooted POSIX-looking one.
        if (remainder.size() >= 3 && remainder[0] == '/' && isDriveLetterPath(remainder.substr(1)))
            return remainder.substr(1);

        return remainder;
    }

} // namespace

std::optional<FileUrl> parseFileUrl(std::string url)
{
    auto parsed = FileUrl { .url = {}, .host = {}, .path = {} };
    if (url.starts_with(Prefix))
    {
        // A query or fragment (RFC 3986 §3) is no part of the path: file:///src/x.cpp#L10 names x.cpp, as
        // the desktop's URL handler reads it. A '?' or '#' that is part of a name travels escaped.
        auto const whole = std::string_view(url).substr(Prefix.size());
        auto const remainder = whole.substr(0, whole.find_first_of("?#"));
        parsed.host = std::string { authorityOf(remainder) };
        // A URL's path is percent-encoded -- the only way a directory name holding ESC or BEL can travel
        // in OSC 7 without ending it -- and read decoded, as foot, Konsole, kitty and WezTerm read it.
        parsed.path = core::unescapeURL(encodedPathOf(remainder));
    }
    else
        parsed.path = url;

    if (parsed.path.contains('\0'))
        return std::nullopt;
    parsed.url = std::move(url);
    return parsed;
}

auto localPathOfFileUrl(std::string const& url, std::string_view localHost, PathConvention convention)
    -> std::optional<std::string>
{
    return parseFileUrl(url).and_then([localHost, convention](FileUrl parsed) -> std::optional<std::string> {
        // A different host is a remote working directory; no path at all names nothing to open.
        if (!isLocalHost(parsed.host, localHost) || parsed.path.empty()
            || !namesAPathUnder(parsed.path, convention))
            return std::nullopt;
        return std::move(parsed.path);
    });
}

} // namespace vtbackend
