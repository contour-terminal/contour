// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockPaths.hpp>

#include <vtbackend/core/WorkingDirectory.hpp>

namespace vtbackend
{

namespace
{
    /// @return Whether @p directory names one provably on this machine, as CwdPurpose::OpenLocally demands.
    [[nodiscard]] bool isLocalDirectory(WorkingDirectorySnapshot const& directory) noexcept
    {
        return usableFor(CwdPurpose::OpenLocally, directory.locality) && !directory.path.empty();
    }
} // namespace

bool ranInLocalDirectory(CommandBlockRecord const& block) noexcept
{
    return isLocalDirectory(block.workingDirectory);
}

std::expected<PathBases, PathRefusal> localPathBaseFor(CommandBlockRecord const* block,
                                                       TerminalDirectory const& terminal)
{
    if (block == nullptr)
        return terminal.openLocally.empty() ? PathBases {} : PathBases { terminal.openLocally };

    // Printed on another machine: the command started there, a shell there reported its directory while the
    // block was open, or, still printing, the block now talks from there.
    auto const stillPrinting = block->state != CommandBlockState::Finished;
    if (block->workingDirectory.locality == ContextLocality::Foreign
        || block->foreignDirectory == ForeignDirectory::Reported
        || (stillPrinting && terminal.locality == ContextLocality::Foreign))
        return std::unexpected(PathRefusal::RanElsewhere);

    auto bases = PathBases {};
    auto const& start = ranInLocalDirectory(*block) ? block->workingDirectory.path : terminal.openLocally;
    if (!start.empty())
        bases.push_back(start);
    // Where the command left the shell, for what it printed after a `cd` of its own: `cd build && make`.
    if (isLocalDirectory(block->endWorkingDirectory) && block->endWorkingDirectory.path != start)
        bases.push_back(block->endWorkingDirectory.path);
    return bases;
}

} // namespace vtbackend
