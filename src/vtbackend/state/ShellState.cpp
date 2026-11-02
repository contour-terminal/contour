// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/ShellState.hpp>

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace vtbackend
{

namespace
{
    using detail::StateFailure;
} // namespace

// {{{ CommandBlockStore

CommandBlockStoreState CommandBlockStore::captureState() const
{
    auto state = CommandBlockStoreState {
        .records = std::vector<CommandBlockRecord>(_records.begin(), _records.end()),
        .changeStamps = std::vector<uint64_t>(_changeStamps.begin(), _changeStamps.end()),
        .runningFor = {},
        .awaitedCommandContextParent = _awaitedCommandContextParent,
        .nextId = _nextId,
        .idEpoch = _idEpoch,
        .revision = _revision,
        .contentRevision = _contentRevision,
    };
    if (_runningSince)
        state.runningFor =
            std::chrono::duration_cast<std::chrono::nanoseconds>(_steadyClock.now() - *_runningSince);
    return state;
}

std::expected<void, StateDecodeError> CommandBlockStore::validateState(
    CommandBlockStoreState const& state) const
{
    auto const ascendingIds =
        std::ranges::adjacent_find(state.records, std::greater_equal<> {}, &CommandBlockRecord::id)
        == state.records.end();
    if (state.records.size() > _limits.maxRecords || state.changeStamps.size() != state.records.size()
        || !ascendingIds
        || std::ranges::any_of(state.records, [](CommandBlockRecord const& record) { return !record.id; })
        || state.nextId == 0 || state.nextId > std::numeric_limits<IdValue>::max())
        return StateFailure { StateDecodeError::InvalidValue };
    return {};
}

std::expected<void, StateDecodeError> CommandBlockStore::restoreState(CommandBlockStoreState const& state)
{
    if (auto const valid = validateState(state); !valid)
        return valid;
    _records = std::deque<CommandBlockRecord>(state.records.begin(), state.records.end());
    _changeStamps = std::deque<uint64_t>(state.changeStamps.begin(), state.changeStamps.end());
    _runningSince.reset();
    if (state.runningFor)
        _runningSince =
            _steadyClock.now()
            - std::chrono::duration_cast<core::platform::SteadyTimePoint::duration>(*state.runningFor);
    _awaitedCommandContextParent = state.awaitedCommandContextParent;
    _nextId = static_cast<IdValue>(state.nextId);
    _idEpoch = state.idEpoch;
    _revision = state.revision;
    _contentRevision = state.contentRevision;
    return {};
}

// }}}

// {{{ SemanticBlockTracker

SemanticBlockTrackerState SemanticBlockTracker::captureState() const
{
    return SemanticBlockTrackerState { .token = _token,
                                       .hiddenThrough = _hiddenThrough,
                                       .hiddenEpoch = _hiddenEpoch };
}

std::expected<void, StateDecodeError> SemanticBlockTracker::validateState(
    SemanticBlockTrackerState const& state)
{
    if (state.token
        && std::ranges::any_of(*state.token, [](uint16_t part) { return part > MaxSemanticBlockTokenPart; }))
        return StateFailure { StateDecodeError::InvalidValue };
    return {};
}

std::expected<void, StateDecodeError> SemanticBlockTracker::restoreState(
    SemanticBlockTrackerState const& state)
{
    if (auto const valid = validateState(state); !valid)
        return valid;
    _token = state.token;
    _hiddenThrough = state.hiddenThrough;
    _hiddenEpoch = state.hiddenEpoch;
    return {};
}

// }}}

// {{{ MarkArbiter

MarkArbiterState MarkArbiter::captureState() const noexcept
{
    return MarkArbiterState { .owner = _owner,
                              .completedCycles = _completedCycles,
                              .hasOpenCommand = _hasOpenCommand };
}

void MarkArbiter::restoreState(MarkArbiterState const& state) noexcept
{
    _owner = state.owner;
    _completedCycles = state.completedCycles;
    _hasOpenCommand = state.hasOpenCommand;
}

// }}}

// {{{ ContextStack

ContextStackState ContextStack::captureState() const
{
    auto state = ContextStackState { .records = {},
                                     .chain = {},
                                     .nextId = _nextId.value,
                                     .revision = _revision,
                                     .droppedPushes = _droppedPushes };
    for (auto const id: _creationOrder)
        if (auto const it = _byId.find(id); it != _byId.end())
            state.records.push_back(*it->second);
    for (auto const& entry: _chain)
        state.chain.push_back(entry.record->id);
    return state;
}

std::expected<void, StateDecodeError> ContextStack::validateState(ContextStackState const& state) const
{
    auto ids = std::unordered_set<ContextId> {};
    if (state.records.size() > _limits.maxRetained || state.chain.size() > _limits.maxDepth
        || !std::ranges::all_of(state.records,
                                [&](TerminalContext const& record) {
                                    return record.id != ContextId {} && ids.insert(record.id).second;
                                })
        || !std::ranges::all_of(state.chain, [&](ContextId id) { return ids.contains(id); }))
        return StateFailure { StateDecodeError::InvalidValue };
    return {};
}

std::expected<void, StateDecodeError> ContextStack::restoreState(ContextStackState const& state)
{
    if (auto const valid = validateState(state); !valid)
        return valid;
    auto byId = std::unordered_map<ContextId, std::shared_ptr<TerminalContext>> {};
    auto creationOrder = std::deque<ContextId> {};
    for (auto const& record: state.records)
    {
        if (!record.id || !byId.emplace(record.id, std::make_shared<TerminalContext>(record)).second)
            return StateFailure { StateDecodeError::InvalidValue };
        creationOrder.push_back(record.id);
    }
    auto chain = std::vector<Entry> {};
    for (auto const id: state.chain)
    {
        auto const it = byId.find(id);
        if (it == byId.end())
            return StateFailure { StateDecodeError::InvalidValue };
        chain.push_back(Entry { .record = it->second });
    }

    _byId = std::move(byId);
    _creationOrder = std::move(creationOrder);
    _chain = std::move(chain);
    _nextId = ContextId { state.nextId };
    _revision = state.revision;
    _droppedPushes = static_cast<size_t>(state.droppedPushes);
    return {};
}

// }}}

} // namespace vtbackend
