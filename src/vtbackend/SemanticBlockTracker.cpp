// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/SemanticBlockTracker.hpp>

#include <random>

namespace vtbackend
{

SemanticBlockTracker::SemanticBlockTracker(size_t maxBlocks): _maxBlocks { maxBlocks }
{
}

void SemanticBlockTracker::setEnabled(bool enabled)
{
    _enabled = enabled;
    if (enabled)
    {
        _token = generateToken();
    }
    else
    {
        _token.reset();
        _completedBlocks.clear();
        _currentBlock.reset();
    }
}

bool SemanticBlockTracker::isEnabled() const noexcept
{
    return _enabled;
}

std::optional<SemanticBlockTracker::Token> const& SemanticBlockTracker::token() const noexcept
{
    return _token;
}

bool SemanticBlockTracker::validateToken(Token const& candidate) const noexcept
{
    return _token.has_value() && _token.value() == candidate;
}

SemanticBlockTracker::Token SemanticBlockTracker::generateToken()
{
    auto rd = std::random_device {};
    auto token = Token {};
    for (auto& part: token)
        part = static_cast<uint16_t>(rd());
    return token;
}

void SemanticBlockTracker::promptStart()
{
    if (!_enabled)
        return;

    // If there's a current block that was finished, push it to completed.
    // Unfinished blocks (e.g. from Ctrl+C before OSC 133;D) are intentionally
    // discarded — only commands that ran to completion have reliable metadata.
    if (_currentBlock && _currentBlock->finished)
    {
        _completedBlocks.push_back(std::move(*_currentBlock));
        if (_completedBlocks.size() > _maxBlocks)
            _completedBlocks.pop_front();
    }

    // Start tracking a new block (replaces any unfinished block).
    _currentBlock = CommandBlockInfo {};
}

void SemanticBlockTracker::commandOutputStart(std::optional<std::string> const& commandLine)
{
    if (!_enabled)
        return;

    if (!_currentBlock)
        _currentBlock = CommandBlockInfo {};

    _currentBlock->commandLine = commandLine;
}

void SemanticBlockTracker::commandFinished(int exitCode)
{
    if (!_enabled)
        return;

    if (!_currentBlock)
        _currentBlock = CommandBlockInfo {};

    _currentBlock->exitCode = exitCode;
    _currentBlock->finished = true;
}

std::deque<CommandBlockInfo> const& SemanticBlockTracker::completedBlocks() const noexcept
{
    return _completedBlocks;
}

std::optional<CommandBlockInfo> const& SemanticBlockTracker::currentBlock() const noexcept
{
    return _currentBlock;
}

} // namespace vtbackend
