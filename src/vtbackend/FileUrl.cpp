// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/FileUrl.hpp>

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

    /// The authority of a file:// URL's remainder (what follows "file://"): everything up to the first
    /// '/'. Empty for the rooted file:///path form and for a Windows drive-letter path.
    [[nodiscard]] constexpr std::string_view authorityOf(std::string_view remainder) noexcept
    {
        if (remainder.empty() || remainder.front() == '/' || isDriveLetterPath(remainder))
            return {};
        return remainder.substr(0, remainder.find('/'));
    }
} // namespace

auto extractPathFromFileUrl(std::string const& url) -> std::string
{
    if (!url.starts_with(Prefix))
        return url;
    auto remainder = url.substr(Prefix.size());

    // file:///path -> /path  ;  file://host/path -> /path  ;  file://C:/path -> C:/path
    if (!remainder.empty() && remainder[0] != '/')
    {
        if (isDriveLetterPath(remainder))
            return remainder;
        if (auto const pos = remainder.find('/'); pos != std::string::npos)
        {
            // file://host/C:/path -> C:/path : strip the leading slash before a Windows drive
            // letter so a host-qualified URL still yields a valid native absolute path.
            auto pathPart = remainder.substr(pos);
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

auto localWorkingDirectory(std::string const& url, std::string_view localHost) -> std::optional<std::string>
{
    if (url.starts_with(Prefix)
        && !isLocalHost(authorityOf(std::string_view(url).substr(Prefix.size())), localHost))
        return std::nullopt; // a different host: this is a remote working directory

    auto path = extractPathFromFileUrl(url);
    if (path.empty())
        return std::nullopt;
    return path;
}

} // namespace vtbackend
