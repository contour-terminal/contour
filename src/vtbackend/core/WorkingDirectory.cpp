// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/WorkingDirectory.hpp>

#include <vtbackend/core/FileUrl.hpp>

using std::optional;
using std::string;

namespace vtbackend
{

namespace
{
    /// Whether @p purpose will actually open or chdir into the path, and therefore needs it to be on
    /// this machine.
    [[nodiscard]] constexpr bool needsLocalPath(CwdPurpose purpose) noexcept
    {
        return purpose != CwdPurpose::Display;
    }
} // namespace

optional<ResolvedWorkingDirectory> resolveWorkingDirectory(ContextStack const& contexts,
                                                           string const& osc7Url,
                                                           LocalIdentity const& self,
                                                           CwdPurpose purpose)
{
    // 1. The OSC 3008 ancestry, which is the only source that carries enough identity to say whether
    //    the path it names is on this machine.
    if (auto const context = contexts.effectiveWorkingDirectory(self))
    {
        auto const usable = !needsLocalPath(purpose) || context->locality == ContextLocality::Local;
        if (usable)
            return ResolvedWorkingDirectory { .path = string { context->path },
                                              .source = CwdSource::ContextSignal,
                                              .locality = context->locality };
        // Not usable for THIS purpose, but the fall-through below is not a second chance at the same
        // question: OSC 7 is a different source with its own host authority to test.
    }

    // 2. OSC 7. Its authority is the only locality evidence it carries, which is exactly why an OSC
    //    3008 `cwd=` -- a bare path with no authority at all -- must not be run through the same test:
    //    isLocalHost() accepts an empty authority, so every container path would pass it.
    if (osc7Url.empty())
        return std::nullopt;

    // Asked ONCE, and the answer carries both halves: localWorkingDirectory() already extracts the path
    // on its way to the verdict, so testing locality and then extracting again would parse the same URL
    // twice and throw one of the two strings away.
    if (auto local = localWorkingDirectory(osc7Url, self.hostname))
        return ResolvedWorkingDirectory { .path = std::move(*local),
                                          .source = CwdSource::Osc7,
                                          .locality = ContextLocality::Local };

    if (needsLocalPath(purpose))
        return std::nullopt;

    auto path = extractPathFromFileUrl(osc7Url);
    if (path.empty())
        return std::nullopt;
    return ResolvedWorkingDirectory { .path = std::move(path),
                                      .source = CwdSource::Osc7,
                                      .locality = ContextLocality::Foreign };
}

} // namespace vtbackend
