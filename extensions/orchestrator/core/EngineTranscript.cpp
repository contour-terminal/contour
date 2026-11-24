// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Agents.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/EngineDetail.hpp>
#include <orchestrator/core/Transcript.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    using engine::detail::judge;
    using engine::detail::TranscriptTagSuffix;
    using engine::detail::Verdict;

    /// The declared command a page is read with.
    constexpr auto TranscriptCommand = "agent-transcript"sv;

    /// Why a page that the verb printed could not be read.
    constexpr auto NotAPage = "contour printed no transcript page"sv;

    /// Why the transcript of a run whose agent keeps none Contour reads is not asked for.
    constexpr auto NoReadableTranscript = "Copilot runs keep no transcript Contour can read"sv;
} // namespace

TranscriptView const* Engine::transcript(std::string_view tab, std::string_view run) const noexcept
{
    auto const view = std::ranges::find_if(
        _transcripts, [&](TranscriptView const& kept) { return kept.tab == tab && kept.run == run; });
    return view != _transcripts.end() ? &*view : nullptr;
}

TranscriptView* Engine::findTranscript(std::string_view tab, std::string_view run) noexcept
{
    auto const view = std::ranges::find_if(
        _transcripts, [&](TranscriptView const& kept) { return kept.tab == tab && kept.run == run; });
    return view != _transcripts.end() ? &*view : nullptr;
}

bool Engine::isTranscriptShown(TranscriptView const& view) const noexcept
{
    auto const shown = _transcriptShown.find(view.tab);
    return shown != _transcriptShown.end() && shown->second == view.run;
}

void Engine::dropUnshownTranscripts()
{
    // The oldest go first; one an open tab shows stays, however many there are.
    auto excess = _transcripts.size() > MaxCachedTranscripts ? _transcripts.size() - MaxCachedTranscripts : 0;
    std::erase_if(_transcripts, [&](TranscriptView const& view) {
        if (excess == 0 || isTranscriptShown(view))
            return false;
        --excess;
        return true;
    });
}

namespace
{
    /// @return Whether @p view shows the page it asks for: read, or read again while the page it held stays
    ///         drawn. An empty page is left out, since what stands in for it says whether it is being read.
    [[nodiscard]] bool showsItsPage(TranscriptView const& view) noexcept
    {
        return (view.state == FetchState::Loaded || view.state == FetchState::Loading) && view.content
               && view.content->page == view.page && !view.content->entries.empty();
    }

    /// @return Whether @p before and @p after draw the same: both show the same page, or neither shows one
    /// and
    ///         they agree on why.
    [[nodiscard]] bool isDrawnAlike(TranscriptView const& before, TranscriptView const& after)
    {
        if (showsItsPage(before) && showsItsPage(after))
            return before.content == after.content;
        return before.state == after.state && before.failure == after.failure
               && before.content == after.content;
    }
} // namespace

void Engine::closeTab(std::string_view tab)
{
    hideBoard(tab);
    if (auto const shown = _transcriptShown.find(tab); shown != _transcriptShown.end())
        _transcriptShown.erase(shown);
    if (std::erase_if(_transcripts, [&](TranscriptView const& view) { return view.tab == tab; }) > 0)
        ++_revision;
}

void Engine::readTranscript(ReadTranscript const& request, std::vector<Effect>& out)
{
    auto* const run = findRun(request.run);
    if (!_config || run == nullptr || request.page == 0)
        return;
    // The tab shows this run's transcript now; the one asked for last moves to the back, and a fifth drops
    // the one asked for longest ago that no tab shows.
    _transcriptShown.insert_or_assign(request.tab, request.run);
    auto view = TranscriptView { .run = request.run,
                                 .tab = request.tab,
                                 .page = request.page,
                                 .state = FetchState::Loading,
                                 .content = std::nullopt,
                                 .failure = {} };
    // A page read again while it is shown, as a followed transcript is every few seconds, is drawn as it was
    // until the new one comes: the dashboard has nothing to redraw for it.
    auto rereads = false;
    if (auto const kept = std::ranges::find_if(
            _transcripts,
            [&](TranscriptView const& held) { return held.tab == request.tab && held.run == request.run; });
        kept != _transcripts.end())
    {
        rereads = kept->page == request.page && showsItsPage(*kept);
        view.content = std::move(kept->content);
        _transcripts.erase(kept);
    }
    // Only Claude Code's log is read: an agent that reads none of its state fails at once, with no call.
    auto const readable = agentRowOf(run->agent).claudeState == ClaudeState::Read;
    if (!readable)
    {
        view.state = FetchState::Failed;
        view.failure = std::string { NoReadableTranscript };
    }
    _transcripts.push_back(std::move(view));
    dropUnshownTranscripts();
    if (!rereads)
        ++_revision;
    if (std::holds_alternative<Finished>(run->state))
        feed(request.run, MarkSeen {}, out);
    // One page of a run is read at a time: a page asked for meanwhile, here or in another tab, is read once
    // that one answers.
    if (readable && !_transcriptCalls.contains(request.run))
        if (auto const* const current = findRun(request.run))
            issueTranscript(*current, request.tab, request.page, out);
}

void Engine::issueTranscript(Run const& run, std::string_view tab, uint32_t page, std::vector<Effect>& out)
{
    _transcriptCalls.insert_or_assign(run.id, TranscriptCall { .tab = std::string { tab }, .page = page });
    auto arguments = std::vector<Argument> {
        Argument { .name = "session", .value = run.session },
        Argument { .name = "path", .value = run.worktree },
        Argument { .name = "page", .value = std::format("{}", page) },
        Argument { .name = "format", .value = std::format("{}", TranscriptFormat) },
    };
    // The run's agent read its state where its set pointed it, which the read must point to as well.
    if (auto set = envSetArgumentOf(run))
        arguments.push_back(std::move(*set));
    // In the worktree root: the run's own worktree may be gone, its log is not.
    issue(RunCommand { .tag = std::format("{}{}", run.id, TranscriptTagSuffix),
                       .command = TranscriptCommand,
                       .arguments = std::move(arguments),
                       .cwd = _config->worktreeRoot() },
          Priority::Poll,
          out);
}

void Engine::issueWaitingTranscript(std::string_view runId, std::vector<Effect>& out)
{
    auto const* const run = findRun(runId);
    if (run == nullptr)
        return;
    auto const waiting = std::ranges::find_if(_transcripts, [&](TranscriptView const& view) {
        return view.run == runId && view.state == FetchState::Loading;
    });
    if (waiting != _transcripts.end())
        issueTranscript(*run, waiting->tab, waiting->page, out);
}

void Engine::onTranscriptResult(std::string_view runId,
                                ProcessOutcome const& outcome,
                                std::vector<Effect>& out)
{
    auto const call = _transcriptCalls.find(runId);
    if (call == _transcriptCalls.end())
        return;
    auto const asked = std::move(call->second);
    _transcriptCalls.erase(call);
    // The page answered goes to the tab that asked, while it still waits for that page; then the next page
    // waiting for this run, in any tab, is asked for.
    auto const waitsForIt = [&](TranscriptView const& view) {
        return view.run == runId && view.page == asked.page && view.state == FetchState::Loading;
    };
    auto* view = findTranscript(asked.tab, runId);
    if (view == nullptr)
    {
        // The tab that asked dropped it (it closed): a tab that still shows the run and waits for the same
        // page takes it, rather than having it read again.
        auto const other = std::ranges::find_if(_transcripts, [&](TranscriptView const& candidate) {
            return waitsForIt(candidate) && isTranscriptShown(candidate);
        });
        view = other != _transcripts.end() ? &*other : nullptr;
    }
    if (view == nullptr || !waitsForIt(*view))
    {
        issueWaitingTranscript(runId, out);
        return;
    }
    // A page that came back as it was shown changes nothing drawn: a followed agent that wrote nothing new
    // costs no redraw.
    auto const previous = *view;
    applyTranscriptOutcome(*view, outcome);
    if (!isDrawnAlike(previous, *view))
        ++_revision;
    issueWaitingTranscript(runId, out);
}

void Engine::applyTranscriptOutcome(TranscriptView& view, ProcessOutcome const& outcome)
{
    auto const judgement = judge(outcome);
    if (judgement.verdict != Verdict::Ok)
    {
        auto const mismatch =
            judgement.verdict == Verdict::Failed
            && classifyTranscriptFailure(outcome.stderrText) == TranscriptReadError::FormatMismatch;
        view.state = mismatch ? FetchState::FormatMismatch : FetchState::Failed;
        view.failure = mismatch ? std::string {} : judgement.why;
        return;
    }
    auto page = parseTranscriptPage(outcome.stdoutText);
    if (!page)
    {
        auto const mismatch = page.error() == TranscriptReadError::FormatMismatch;
        view.state = mismatch ? FetchState::FormatMismatch : FetchState::Failed;
        view.failure = mismatch ? std::string {} : std::string { NotAPage };
        return;
    }
    view.state = FetchState::Loaded;
    view.failure.clear();
    view.content = std::move(*page);
}

} // namespace orchestrator
