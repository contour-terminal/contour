// SPDX-License-Identifier: Apache-2.0
#include <core/tui/CommandRegistry.hpp>

#include <algorithm>

namespace core::tui
{

void CommandRegistry::add(CommandEntry entry)
{
    // Replace existing entry with same ID
    for (auto& existing: _commands)
    {
        if (existing.id == entry.id)
        {
            existing = std::move(entry);
            return;
        }
    }
    _commands.push_back(std::move(entry));
}

void CommandRegistry::remove(std::string_view id)
{
    std::erase_if(_commands, [id](auto const& entry) { return entry.id == id; });
}

auto CommandRegistry::commands() const -> std::span<CommandEntry const>
{
    return _commands;
}

auto CommandRegistry::commandsForContext(CommandContext ctx) const -> std::vector<CommandEntry const*>
{
    auto result = std::vector<CommandEntry const*> {};
    for (auto const& entry: _commands)
    {
        if (entry.context == ctx || entry.context == CommandContext::Both)
            result.push_back(&entry);
    }
    return result;
}

auto CommandRegistry::find(std::string_view id) const -> CommandEntry const*
{
    for (auto const& entry: _commands)
    {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

auto CommandRegistry::size() const noexcept -> std::size_t
{
    return _commands.size();
}

auto CommandRegistry::empty() const noexcept -> bool
{
    return _commands.empty();
}

} // namespace core::tui
