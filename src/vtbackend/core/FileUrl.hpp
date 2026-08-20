// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/ASCII.hpp>

#include <algorithm>
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

    /// Compares two host labels case-insensitively. @see crispy::ascii::fold
    [[nodiscard]] constexpr bool sameHostLabel(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(
            a, b, [](char x, char y) { return crispy::ascii::fold(x) == crispy::ascii::fold(y); });
    }
} // namespace detail

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

    auto const label = detail::bareHostLabel(host);
    return detail::sameHostLabel(label, "localhost")
           || detail::sameHostLabel(label, detail::bareHostLabel(localHost));
}

/// Extracts a local filesystem path from a file:// URL (as set by OSC 7).
/// Returns the URL unchanged if it does not start with "file://".
[[nodiscard]] auto extractPathFromFileUrl(std::string const& url) -> std::string;

/// The local filesystem path a working-directory URL points at, or nullopt when the URL names a host
/// other than @p localHost -- a remote (e.g. SSH) working directory that does not exist on this machine.
///
/// OSC 7 reports the working directory as file://HOST/PATH. A host @ref isLocalHost accepts is this
/// machine; the returned path has the file:// scheme, the host authority and the leading "//" stripped
/// (see @ref extractPathFromFileUrl). Any other host is remote and yields nullopt, as does a URL that
/// carries no path at all.
///
/// @param url       The working-directory URL: an OSC 7 file:// URL, or a bare local path.
/// @param localHost This machine's host name.
/// @return The local path to open, or nullopt when @p url is remote or path-less.
[[nodiscard]] auto localWorkingDirectory(std::string const& url, std::string_view localHost)
    -> std::optional<std::string>;

} // namespace vtbackend
