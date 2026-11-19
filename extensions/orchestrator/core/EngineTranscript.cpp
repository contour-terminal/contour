// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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
} // namespace

TranscriptView const* Engine::transcript(std::string_view run) const noexcept
{
    auto const view = std::ranges::find(_transcripts, run, &TranscriptView::run);
    return view != _transcripts.end() ? &*view : nullptr;
}

TranscriptView* Engine::findTranscript(std::string_view run) noexcept
{
    auto const view = std::ranges::find(_transcripts, run, &TranscriptView::run);
    return view != _transcripts.end() ? &*view : nullptr;
}

void Engine::readTranscript(ReadTranscript const& request, std::vector<Effect>& out)
{
    auto* const run = findRun(request.run);
    if (!_config || run == nullptr || request.page == 0)
        return;
    // The one asked for last moves to the back; a fifth run's drops the one asked for longest ago.
    auto view = TranscriptView { .run = request.run,
                                 .page = request.page,
                                 .state = FetchState::Loading,
                                 .content = std::nullopt,
                                 .failure = {} };
    if (auto const kept = std::ranges::find(_transcripts, request.run, &TranscriptView::run);
        kept != _transcripts.end())
    {
        view.content = std::move(kept->content);
        _transcripts.erase(kept);
    }
    _transcripts.push_back(std::move(view));
    if (_transcripts.size() > MaxCachedTranscripts)
        _transcripts.erase(_transcripts.begin());
    ++_revision;
    if (std::holds_alternative<Finished>(run->state))
        feed(request.run, MarkSeen {}, out);
    // A page asked for while another is read is asked for once that one answers.
    if (!_transcriptCalls.contains(request.run))
        if (auto const* const current = findRun(request.run))
            issueTranscript(*current, request.page, out);
}

void Engine::issueTranscript(Run const& run, uint32_t page, std::vector<Effect>& out)
{
    _transcriptCalls.insert_or_assign(run.id, page);
    // In the worktree root: the run's own worktree may be gone, its log is not.
    issue(RunCommand { .tag = std::format("{}{}", run.id, TranscriptTagSuffix),
                       .command = TranscriptCommand,
                       .arguments = { Argument { .name = "session", .value = run.session },
                                      Argument { .name = "path", .value = run.worktree },
                                      Argument { .name = "page", .value = std::format("{}", page) },
                                      Argument { .name = "format",
                                                 .value = std::format("{}", TranscriptFormat) } },
                       .cwd = _config->worktreeRoot() },
          Priority::Poll,
          out);
}

void Engine::onTranscriptResult(std::string_view runId,
                                ProcessOutcome const& outcome,
                                std::vector<Effect>& out)
{
    auto const call = _transcriptCalls.find(runId);
    if (call == _transcriptCalls.end())
        return;
    auto const asked = call->second;
    _transcriptCalls.erase(call);
    auto* const view = findTranscript(runId);
    if (view == nullptr)
        return;
    if (view->page != asked)
    {
        if (auto const* const run = findRun(runId))
            issueTranscript(*run, view->page, out);
        return;
    }
    ++_revision;
    auto const judgement = judge(outcome);
    if (judgement.verdict != Verdict::Ok)
    {
        auto const mismatch =
            judgement.verdict == Verdict::Failed
            && classifyTranscriptFailure(outcome.stderrText) == TranscriptReadError::FormatMismatch;
        view->state = mismatch ? FetchState::FormatMismatch : FetchState::Failed;
        view->failure = mismatch ? std::string {} : judgement.why;
        return;
    }
    auto page = parseTranscriptPage(outcome.stdoutText);
    if (!page)
    {
        auto const mismatch = page.error() == TranscriptReadError::FormatMismatch;
        view->state = mismatch ? FetchState::FormatMismatch : FetchState::Failed;
        view->failure = mismatch ? std::string {} : std::string { NotAPage };
        return;
    }
    view->state = FetchState::Loaded;
    view->failure.clear();
    view->content = std::move(*page);
}

} // namespace orchestrator
