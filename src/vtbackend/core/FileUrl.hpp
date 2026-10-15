// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/ASCII.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Everything the terminal needs to read a `file://` URL: the two places an application hands us one are
// OSC 7 (the working directory) and OSC 8 (a hyperlink target), and both ask the same two questions --
// does this URL name THIS machine, and what local path does it mean?
//
// A leaf header on purpose. These are string decisions with no dependency on the grid, so they must not
// drag the hyperlink cache (or HintModeHandler's <regex>) into the code that merely resolves a path.

namespace vtbackend
{

namespace detail
{
    /// The DNS label before the first '.': the bare machine name of a possibly-qualified host, so
    /// "fedora" and "fedora.corp.example" share one.
    [[nodiscard]] constexpr std::string_view bareHostLabel(std::string_view host) noexcept
    {
        return host.substr(0, host.find('.'));
    }
} // namespace detail

/// Whether @p a and @p b spell one host name, compared case-insensitively as DNS compares names.
/// @see crispy::ascii::fold
/// @param a A host name, or one of its labels.
/// @param b Another.
/// @return Whether the two are equal but for case.
[[nodiscard]] constexpr bool sameHostName(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(
        a, b, [](char x, char y) { return crispy::ascii::fold(x) == crispy::ascii::fold(y); });
}

/// Whether @p host is the loopback name every machine answers to: "localhost", in any case, bare or
/// qualified ("localhost.localdomain").
///
/// Such a name means the machine that reads it. In a URL a program on this machine wrote, that is this
/// machine; in an identity another machine reports, it names no machine at all.
/// @param host A host name or a URL's authority.
/// @return Whether @p host is a loopback name.
[[nodiscard]] constexpr bool isLoopbackHost(std::string_view host) noexcept
{
    return sameHostName(detail::bareHostLabel(host), "localhost");
}

/// Whether @p host -- the authority of a file:// URL -- names the machine whose host name is @p localHost.
///
/// True for an empty authority, for "localhost", and for a host sharing its first DNS label with
/// @p localHost, compared case-insensitively. Applications build a file:// URL from gethostname(2), which
/// on one machine reports the short name and on another the fully-qualified one, so "darkleon",
/// "DARKLEON" and "darkleon.lan" must all name a machine calling itself either "darkleon" or
/// "darkleon.lan" -- an exact match rejects the very forms the C library hands out.
///
/// The local host name is a parameter rather than read here so the decision stays pure and unit-testable
/// (and so vtbackend stays Qt-free); the caller injects it (QHostInfo::localHostName() in the GUI).
///
/// @param host      The URL's authority, empty when it carries none.
/// @param localHost This machine's host name.
/// @return Whether @p host is this machine.
[[nodiscard]] constexpr bool isLocalHost(std::string_view host, std::string_view localHost) noexcept
{
    if (host.empty())
        return true;

    return isLoopbackHost(host)
           || sameHostName(detail::bareHostLabel(host), detail::bareHostLabel(localHost));
}

/// A file:// URL -- or a bare path standing in for one -- as an application reported it, read once.
///
/// OSC 7 (the working directory) and OSC 8 (a hyperlink target) hand the terminal such a URL; every reader
/// wants its host or its path, so it is parsed where it arrives rather than by each reader again.
struct FileUrl
{
    std::string url;  ///< As reported, unchanged: what the wire and a mirror carry. Empty for none.
    std::string host; ///< The authority; empty for none (file:///path, a drive letter, a bare path).
    std::string
        path; ///< Percent-decoded, without query or fragment (a bare path as it is); never holds a NUL.

    bool operator==(FileUrl const&) const = default;
};

/// Reads @p url: a file:// URL, whose path ends at a query or fragment and is percent-decoded byte by byte
/// (a malformed escape is kept as written, @see core::unescapeURL), or anything else, taken as a bare path
/// that is not encoded.
///
/// A path holding a NUL is refused: no path can, as every filesystem call would read it cut short at the
/// NUL, as a different file.
/// @param url The URL as reported.
/// @return The URL with its host and decoded path, or nullopt when its path holds a NUL.
[[nodiscard]] std::optional<FileUrl> parseFileUrl(std::string url);

/// How a machine spells an absolute local path, which decides whether a drive-letter path names a file on it.
enum class PathConvention : uint8_t
{
    Posix = 0, ///< Rooted at "/": C:/x is not even an absolute path, and so names no file here.
    Windows,   ///< A drive letter may lead a path: C:/x is a local file.
};

/// The path convention of the platform this is built for.
inline constexpr auto NativePathConvention =
#ifdef _WIN32
    PathConvention::Windows;
#else
    PathConvention::Posix;
#endif

/// The local filesystem path a file:// URL names, or nullopt when the URL names a host other than
/// @p localHost -- a remote (e.g. SSH) directory or file that does not exist on this machine.
///
/// The one reader of such a URL for whatever opens, shows or spawns into its path: OSC 7's working
/// directory and OSC 8's hyperlink target alike, so a tooltip names the file a click opens. A host
/// @ref isLocalHost accepts is this machine; the returned path is the URL's decoded path, without query or
/// fragment (@see parseFileUrl). Any other host is remote and yields nullopt, as does a URL that carries no
/// path at all, or one holding a NUL -- and one naming a drive-letter path (C:/x) where @p convention gives
/// such a path no meaning.
///
/// @param url        A file:// URL, or a bare local path.
/// @param localHost  This machine's host name.
/// @param convention How this machine spells a path.
/// @return The local path, or nullopt when @p url is remote or path-less.
[[nodiscard]] auto localPathOfFileUrl(std::string const& url,
                                      std::string_view localHost,
                                      PathConvention convention = NativePathConvention)
    -> std::optional<std::string>;

} // namespace vtbackend
