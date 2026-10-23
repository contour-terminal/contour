// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/links/LinkDetectionPolicy.hpp>
#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/links/PathProbe.hpp>
#include <vtbackend/shell/CommandBlockPaths.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vtbackend
{

/// This machine, as printed text names its files.
struct LinkMachine
{
    std::string home;     ///< The home directory, for `~/`; empty when unknown.
    std::string hostname; ///< The host name a `file://` URL of this machine may give; empty when unknown.
};

/// A printed path the probe's cache does not know: where to look, on the probe thread or, for hint mode, at
/// once.
struct PathToProbe
{
    LinkTarget printed;                   ///< The path as printed, which resolvedPathTarget() completes.
    PathQuery query;                      ///< Its candidates, first to last.
    std::vector<std::string> directories; ///< Parallel to query.candidates: the base each was joined to.
};

/// A printed path refused for the row it is on.
struct RefusedPath
{
    LinkTarget printed;    ///< The path as printed.
    PathRefusal reason {}; ///< Its command ran on another machine, or the terminal replicates one.
};

/// A printed path that names nothing here: no place to look, or the probe's cache knows it names nothing.
struct MissingPath
{
    LinkTarget printed; ///< The path as printed.
};

/// No link at all: a `file://` URL naming another machine, or no path.
struct NotALink
{
};

/// What a link found in printed text is, by the one rule detection, a click and hint mode go by (§4.4,
/// §7.6): a URL or a custom link as it opens; a path refused, known from the probe's cache, missing, or to
/// be looked for; or no link.
using PrintedLink = std::variant<LinkTarget, PathToProbe, RefusedPath, MissingPath, NotALink>;

/// Whether @p url is a `file:` URL, by its scheme, whatever its case.
/// @param url A link's target.
/// @return Whether it names a file, rather than what a browser opens.
[[nodiscard]] bool isFileUrl(std::string_view url) noexcept;

/// The category of a link found in printed text, for the detection policy.
/// @param kind How the link was found.
/// @param target The link's target: for a custom pattern, what its URL template expands to.
/// @return FilePath for a path or a `file:` URL (a custom pattern's included), Url for everything else.
[[nodiscard]] LinkCategory linkCategoryOf(LinkKind kind, std::string_view target) noexcept;

/// Classifies a link found in printed text, without touching the filesystem.
///
/// A `file://` URL is the local path it names, a path like any printed one: printed text never reaches the
/// opener of OSC 8 links, which runs a program (§7.6). One naming another machine is no link. A path printed
/// on a row whose command ran on another machine, or on a replica of one, is refused. Any other is placed: a
/// relative path in each of the row's bases, `~/` in the home directory, an absolute one as it is
/// (pathCandidates()). The probe's cache then answers where it knows every candidate.
/// @param printed The link as printed (printedLinkTarget()).
/// @param bases Where relative paths on its row resolve (Terminal::localPathBaseAt()).
/// @param machine This machine: its home directory and its host name.
/// @param probe The probe whose cache answers; null for none, which leaves every path to be looked for.
/// @return What it is.
[[nodiscard]] PrintedLink classifyPrintedLink(LinkTarget printed,
                                              std::expected<PathBases, PathRefusal> const& bases,
                                              LinkMachine const& machine,
                                              PathProbe const* probe);

} // namespace vtbackend
