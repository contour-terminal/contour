// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ForegroundProcess.hpp>

#include <vtpty/ProgramName.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace vtpty
{

namespace
{
    /// The interactive shells, by programStem(): a shell waits for the command it runs, so the walk goes on
    /// below one. Not `wsl` (nor `wslhost`): it runs another system's programs, a relay rather than a shell,
    /// and is named as such.
    constexpr auto Shells = std::array<std::string_view, 14> {
        "bash", "cmd",        "csh",  "dash", "elvish", "fish",  "ksh",
        "nu",   "powershell", "pwsh", "sh",   "tcsh",   "xonsh", "zsh",
    };

    [[nodiscard]] bool isShell(std::string_view name) noexcept
    {
        return std::ranges::any_of(Shells,
                                   [name](std::string_view shell) { return isSameProgram(name, shell); });
    }
} // namespace

std::optional<std::string> foregroundProcessOf(std::span<ProcessEntry const> processes, uint32_t rootPid)
{
    auto const root = std::ranges::find(processes, rootPid, &ProcessEntry::pid);
    if (root == processes.end())
        return std::nullopt;

    auto const* current = &*root;
    // Each step goes to a strictly newer process, so the walk ends; the bound guards a snapshot whose times
    // are all equal.
    for ([[maybe_unused]] auto const step: std::views::iota(size_t { 0 }, processes.size()))
    {
        if (!isShell(current->name))
            break;
        auto const isChild = [current](ProcessEntry const& entry) {
            return entry.parentPid == current->pid && entry.pid != current->pid && entry.creationTime != 0
                   && entry.creationTime > current->creationTime;
        };
        auto children = processes | std::views::filter(isChild);
        auto const newest = std::ranges::max_element(children, {}, &ProcessEntry::creationTime);
        if (newest == children.end())
            break;
        current = &*newest;
    }
    return current->name;
}

} // namespace vtpty
