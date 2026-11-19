// SPDX-License-Identifier: Apache-2.0
#include <vtextension/SelfPrograms.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>

namespace vtextension
{

std::vector<SelfProgram> selfProgramsOf(std::filesystem::path const& executable)
{
    auto programs = std::vector<SelfProgram> {};
    for (auto const name: SelfProgramNames)
        programs.push_back(
            SelfProgram { .name = std::string { name }, .path = core::pathToUtf8(executable) });
    return programs;
}

void resolveSelfProgram(std::vector<std::string>& argv, std::span<SelfProgram const> programs)
{
    if (argv.empty())
        return;
    if (auto const program = std::ranges::find(programs, argv.front(), &SelfProgram::name);
        program != programs.end())
        argv.front() = program->path;
}

} // namespace vtextension
