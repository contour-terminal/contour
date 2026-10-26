// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/WorkingDirectory.hpp>

#include <vtbackend/core/FileUrl.hpp>

#include <string_view>

using std::optional;
using std::string;

namespace vtbackend
{

optional<ResolvedWorkingDirectory> resolveWorkingDirectory(ContextStack const& contexts,
                                                           FileUrl const& osc7,
                                                           LocalIdentity const& self,
                                                           CwdPurpose purpose)
{
    // 1. The OSC 3008 ancestry, which is the only source that carries enough identity to say whether
    //    the path it names is on this machine.
    if (auto const context = contexts.effectiveWorkingDirectory(self))
    {
        if (usableFor(purpose, context->locality))
            return ResolvedWorkingDirectory { .path = string { context->path },
                                              .source = CwdSource::ContextSignal,
                                              .locality = context->locality };
        // Not usable for THIS purpose, but the fall-through below is not a second chance at the same
        // question: OSC 7 is a different source with its own host authority to test.
    }

    // 2. OSC 7. Its authority is the only locality evidence it carries, which is exactly why an OSC
    //    3008 `cwd=` -- a bare path with no authority at all -- must not be run through the same test:
    //    isLocalHost() accepts an empty authority, so every container path would pass it.
    if (osc7.url.empty() || osc7.path.empty())
        return std::nullopt;

    auto const locality =
        isLocalHost(osc7.host, self.hostname) ? ContextLocality::Local : ContextLocality::Foreign;
    if (!usableFor(purpose, locality))
        return std::nullopt;

    return ResolvedWorkingDirectory { .path = osc7.path, .source = CwdSource::Osc7, .locality = locality };
}

string abbreviateHomePath(std::string_view path, std::string_view home)
{
    if (home.empty() || !path.starts_with(home))
        return string { path };

    // The home directory itself.
    if (path.size() == home.size())
        return "~";

    // Only a whole component matches: without this, a home of "/home/bob" would turn "/home/bobby" into
    // "~by" -- a path that reads as real and points somewhere else entirely.
    if (path[home.size()] != '/')
        return string { path };

    return "~" + string { path.substr(home.size()) };
}

string homeAbbreviatedPath(ResolvedWorkingDirectory const& directory, std::string_view home)
{
    if (directory.locality != ContextLocality::Local)
        return directory.path;
    return abbreviateHomePath(directory.path, home);
}

} // namespace vtbackend
