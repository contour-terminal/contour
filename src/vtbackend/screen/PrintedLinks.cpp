// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/PrintedLinks.hpp>

#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/links/LinkUrlTemplate.hpp>
#include <vtbackend/links/PathResolution.hpp>

#include <core/Assert.hpp>

#include <utility>

namespace vtbackend
{

bool isFileUrl(std::string_view url) noexcept
{
    return hasUrlScheme(url, "file");
}

PrintedLink classifyPrintedLink(LinkTarget printed,
                                std::expected<PathBases, PathRefusal> const& bases,
                                LinkMachine const& machine,
                                PathProbe const* probe)
{
    if (printed.kind != LinkKind::Path)
    {
        if (!isFileUrl(printed.target))
            return printed;
        // A scheme reads the same in any case (RFC 3986); the URL reader knows it in lower case only.
        auto local = localPathOfFileUrl("file" + printed.target.substr(4), machine.hostname);
        if (!local)
            return NotALink {};
        printed = LinkTarget { .kind = LinkKind::Path,
                               .target = std::move(*local),
                               .location = std::nullopt,
                               .pathKind = PathKind::Missing,
                               .directory = {} };
    }
    // §4.4: a row whose command ran elsewhere offers no paths, not even an absolute one: it names a file
    // there.
    if (!bases)
        return RefusedPath { .printed = std::move(printed), .reason = bases.error() };
    // With nowhere to look a relative path has no candidate, nor `~/` one without a home directory.
    auto places = pathCandidates(*bases, machine.home, printed.target);
    if (places.paths.empty())
        return MissingPath { .printed = std::move(printed) };
    // One directory per candidate: an answer's candidateIndex names both.
    Require(places.directories.size() == places.paths.size());
    auto query = PathQuery { .candidates = std::move(places.paths) };
    if (auto const known = probe ? probe->cached(query) : std::nullopt)
    {
        if (known->kind == PathKind::Missing)
            return MissingPath { .printed = std::move(printed) };
        return resolvedPathTarget(std::move(printed), *known, places.directories);
    }
    return PathToProbe { .printed = std::move(printed),
                         .query = std::move(query),
                         .directories = std::move(places.directories) };
}

} // namespace vtbackend
