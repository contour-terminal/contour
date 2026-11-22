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

std::vector<ProgramPath> programTableOf(std::span<ProgramPath const> configured,
                                        std::span<SelfProgram const> self)
{
    auto table = std::vector<ProgramPath>(configured.begin(), configured.end());
    for (auto const& program: self)
        table.push_back(ProgramPath { .name = program.name, .path = program.path });
    return table;
}

void resolveProgram(std::vector<std::string>& argv, std::span<ProgramPath const> programs)
{
    if (argv.empty())
        return;
    if (auto const program = std::ranges::find(programs, argv.front(), &ProgramPath::name);
        program != programs.end())
        argv.front() = program->path;
}

} // namespace vtextension
