// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vtbackend
{

/// What a detected link points at, which decides how it is trimmed, resolved and opened.
enum class LinkKind : uint8_t
{
    Url = 0, ///< A built-in URL scheme: http, https, ftp, file, mailto.
    Path,    ///< A filesystem path; it opens only once confirmed to exist locally.
    Custom,  ///< A user pattern; it opens what its URL template expands to.
};

/// Whether a pattern takes part in passive detection, hover and click, or in hint mode only.
enum class LinkActivation : uint8_t
{
    HintOnly = 0, ///< Hint mode offers its matches; nothing is decorated or clickable.
    Clickable,    ///< Detected passively, decorated, and opened by the follow gesture.
};

/// A `:line[:column]` or `(line[,column])` suffix printed after a path.
struct LinkLocation
{
    int line = 1;   ///< One-based line.
    int column = 1; ///< One-based column; 1 when the suffix named a line only.

    bool operator==(LinkLocation const&) const = default;
};

/// A cell inside one ScanText: which of its rows, and which column of that row.
struct ScanPosition
{
    std::size_t row = 0;    ///< Index into the ScanText's rows, top first.
    ColumnOffset column {}; ///< Grid column within that row.

    auto operator<=>(ScanPosition const&) const = default;
};

/// Whether a match may be opened (§7.6).
enum class LinkOpening : uint8_t
{
    Refused = 0, ///< Its printed text chose a scheme outside the built-in URL schemes: it is no link.
    Allowed,     ///< A path, a built-in URL scheme, or a scheme the user's own template wrote.
};

/// One match of a pattern, mapped back to cells.
struct LinkCandidate
{
    LinkKind kind = LinkKind::Url;
    std::size_t patternIndex = 0; ///< The row of the LinkPatterns table that matched.
    std::size_t byteBegin = 0;    ///< Start of the decorated span in the ScanText's text.
    std::size_t byteEnd = 0;      ///< One past its end.
    ScanPosition first;           ///< First cell of the decorated span.
    ScanPosition last;            ///< Last cell of it, inclusive; a wide character's second column included.
    std::string text;             ///< The decorated span as printed.
    std::string target; ///< What opening it opens: the URL, the path as printed, the expanded template.
    std::optional<LinkLocation> location; ///< A path's printed line and column.
    /// Whether @ref target may be opened. Matching for links drops a refused match; hint mode offers it to
    /// copy, and opens nothing.
    LinkOpening opening = LinkOpening::Refused;

    bool operator==(LinkCandidate const&) const = default;
};

/// What a local path names, once looked at.
enum class PathKind : uint8_t
{
    Missing = 0, ///< Nothing by that name, or nothing we may look at.
    File,        ///< A regular file.
    Directory,   ///< A directory.
};

/// A link resolved far enough to open: what the follow gesture, the context menu and `gx` hand an opener.
struct LinkTarget
{
    LinkKind kind = LinkKind::Url;
    std::string target;                    ///< The URL, or the absolute local path.
    std::optional<LinkLocation> location;  ///< A path's line and column.
    PathKind pathKind = PathKind::Missing; ///< For LinkKind::Path: what it names.
    std::string directory; ///< For LinkKind::Path: the directory a relative path resolved against, if any.

    bool operator==(LinkTarget const&) const = default;
};

/// What @p candidate opens, as it was printed: a path before anything was looked up.
/// @param candidate The match.
/// @return Its kind, target and location; for a path, no place on disk yet.
[[nodiscard]] inline LinkTarget printedLinkTarget(LinkCandidate const& candidate)
{
    return LinkTarget { .kind = candidate.kind,
                        .target = candidate.target,
                        .location = candidate.location,
                        .pathKind = PathKind::Missing,
                        .directory = {} };
}

} // namespace vtbackend
