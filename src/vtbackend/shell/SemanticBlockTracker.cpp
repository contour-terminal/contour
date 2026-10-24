// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/SemanticBlockTracker.hpp>

#include <algorithm>
#include <random>
#include <ranges>
#include <utility>

namespace vtbackend
{

SemanticBlockTokenSource randomSemanticBlockTokens()
{
    return [] {
        auto device = std::random_device {};
        return drawSemanticBlockToken(device);
    };
}

SemanticBlockTracker::SemanticBlockTracker(SemanticBlockTokenSource tokens): _tokens { std::move(tokens) }
{
}

void SemanticBlockTracker::enable(CommandBlockStore const& store)
{
    _token = _tokens();

    // A shell enables the mode from its prompt hook, after the prompt's ;A: the command typed at that prompt
    // starts after the session does, so its block stays visible. Only the newest record can be unfinished,
    // so every older one is hidden with the rest.
    auto const* const current = store.current();
    auto const atPrompt = current && current->state == CommandBlockState::Prompting;
    _hiddenThrough = atPrompt ? CommandBlockId { unbox(current->id) - 1 } : store.currentId();
    _hiddenEpoch = store.idEpoch();
}

void SemanticBlockTracker::disable() noexcept
{
    _token.reset();
}

bool SemanticBlockTracker::isEnabled() const noexcept
{
    return _token.has_value();
}

std::optional<SemanticBlockTracker::Token> const& SemanticBlockTracker::token() const noexcept
{
    return _token;
}

bool SemanticBlockTracker::validateToken(Token const& candidate) const noexcept
{
    return _token.has_value() && *_token == candidate;
}

std::vector<CommandBlockRecord const*> SemanticBlockTracker::finishedBlocks(CommandBlockStore const& store,
                                                                            size_t count) const
{
    auto blocks = std::vector<CommandBlockRecord const*> {};
    if (!isEnabled())
        return blocks;

    // Newest first, so the walk ends at the count -- or at the first block this reader may not see: ids
    // ascend, so nothing older is visible either.
    blocks.reserve(std::min(count, store.size()));
    for (auto const& record: store.records() | std::views::reverse)
    {
        if (blocks.size() == count || !isVisible(store, record))
            break;
        if (record.state == CommandBlockState::Finished && record.end == CommandBlockEnd::Reported)
            blocks.push_back(&record);
    }
    return blocks;
}

CommandBlockRecord const* SemanticBlockTracker::inProgressBlock(CommandBlockStore const& store) const noexcept
{
    auto const* current = store.current();
    return (isEnabled() && current && isVisible(store, *current)) ? current : nullptr;
}

CommandBlockRecord const* SemanticBlockTracker::blockById(CommandBlockStore const& store,
                                                          CommandBlockId id) const noexcept
{
    auto const* const record = isEnabled() ? store.find(id) : nullptr;
    return record && isVisible(store, *record) ? record : nullptr;
}

bool SemanticBlockTracker::isVisible(CommandBlockStore const& store,
                                     CommandBlockRecord const& record) const noexcept
{
    // Once the ids have started over, the store holds only blocks minted since: the floor names none of them.
    return store.idEpoch() != _hiddenEpoch || record.id > _hiddenThrough;
}

} // namespace vtbackend
