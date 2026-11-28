// SPDX-License-Identifier: Apache-2.0
#include <vthost/AttentionLedger.hpp>

#include <algorithm>
#include <chrono>

namespace vthost
{

AttentionLedger AttentionLedger::restored(uint64_t lastSerial,
                                          std::span<std::pair<uint64_t, PaneAttention> const> panes)
{
    auto ledger = AttentionLedger {};
    ledger._lastSerial = lastSerial;
    for (auto const& [session, attention]: panes)
    {
        // A serial an image states beyond its own last one is still handed out already: never reuse it.
        ledger._lastSerial = std::max(
            { ledger._lastSerial, attention.activitySerial, attention.bellSerial, attention.finishedSerial });
        ledger._panes.insert_or_assign(session, attention);
    }
    return ledger;
}

AttentionChange AttentionLedger::noteOutput(uint64_t session)
{
    auto& pane = _panes[session];
    if (pane.activity == proto::OutputActivity::Output)
        return AttentionChange::Unchanged;
    pane.activity = proto::OutputActivity::Output;
    pane.activitySerial = nextSerial();
    return AttentionChange::Changed;
}

AttentionChange AttentionLedger::noteBell(uint64_t session)
{
    auto& pane = _panes[session];
    pane.bell = proto::PaneBell::Rang;
    pane.bellSerial = nextSerial();
    return AttentionChange::Changed;
}

AttentionChange AttentionLedger::noteFinished(uint64_t session,
                                              proto::FinishedCommand command,
                                              std::chrono::system_clock::time_point at)
{
    auto& pane = _panes[session];
    pane.finished = std::move(command);
    pane.finishedAt = at;
    pane.finishedSerial = nextSerial();
    return AttentionChange::Changed;
}

AttentionChange AttentionLedger::markSeen(uint64_t session, SeenSerials seen)
{
    auto const found = _panes.find(session);
    if (found == _panes.end())
        return AttentionChange::Unchanged;
    auto& pane = found->second;
    auto const before = pane;
    if (pane.activity == proto::OutputActivity::Output && pane.activitySerial <= seen.activity)
        pane.activity = proto::OutputActivity::None;
    if (pane.bell == proto::PaneBell::Rang && pane.bellSerial <= seen.bell)
        pane.bell = proto::PaneBell::Silent;
    if (pane.finished && pane.finishedSerial <= seen.finished)
    {
        pane.finished.reset();
        pane.finishedAt.reset();
    }
    pane.seenStatusGeneration = std::max(pane.seenStatusGeneration, seen.statusGeneration);
    return pane == before ? AttentionChange::Unchanged : AttentionChange::Changed;
}

void AttentionLedger::forget(uint64_t session)
{
    _panes.erase(session);
}

PaneAttention const* AttentionLedger::of(uint64_t session) const
{
    auto const found = _panes.find(session);
    return found != _panes.end() ? &found->second : nullptr;
}

std::vector<std::pair<uint64_t, PaneAttention>> AttentionLedger::entries() const
{
    return { _panes.begin(), _panes.end() };
}

proto::PaneAttentionState attentionStateOf(uint64_t session, PaneAttention const& attention)
{
    auto const unixMs = [](std::chrono::system_clock::time_point at) {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count());
    };
    return proto::PaneAttentionState { .session = session,
                                       .activity = attention.activity,
                                       .activitySerial = attention.activitySerial,
                                       .bell = attention.bell,
                                       .bellSerial = attention.bellSerial,
                                       .finished = attention.finished,
                                       .finishedSerial = attention.finishedSerial,
                                       .finishedAtUnixMs = attention.finishedAt.transform(unixMs),
                                       .seenStatusGeneration = attention.seenStatusGeneration };
}

} // namespace vthost
