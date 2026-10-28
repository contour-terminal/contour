// SPDX-License-Identifier: Apache-2.0
#include <contour/window/NoticeQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using contour::window::Notice;
using contour::window::NoticeChange;
using contour::window::NoticeKind;
using contour::window::NoticeQueue;
using contour::window::noticeRow;
using contour::window::NoticeRows;
using contour::window::NoticeSurface;
using contour::window::ToastHover;

namespace
{

constexpr auto T0 = NoticeQueue::TimePoint {} + 1h;

[[nodiscard]] Notice notice(NoticeKind kind, std::string text)
{
    return Notice { .kind = kind, .text = std::move(text) };
}

} // namespace

TEST_CASE("NoticeRows: one per kind, in order, every banner with an action", "[notices]")
{
    for (auto const index: std::views::iota(std::size_t { 0 }, NoticeRows.size()))
    {
        auto const& row = NoticeRows[index];
        CHECK(std::to_underlying(row.kind) == index);
        if (row.surface == NoticeSurface::Banner)
            CHECK_FALSE(row.actionText.empty());
    }
}

TEST_CASE("NoticeQueue: a refused drop is a short toast with a mark", "[notices]")
{
    auto const& row = noticeRow(NoticeKind::DropRefused);
    CHECK(row.surface == NoticeSurface::Toast);
    CHECK(row.glyph == "!");
    CHECK(row.duration == 4000ms);
    CHECK(row.action == contour::window::NoticeAction::None);

    // Every drop error that is a refusal has words; they are all safe to show on one row.
    for (auto const& refusal: contour::window::DropRefusalRows)
        if (refusal.error != contour::window::DropTextError::NoPaths)
        {
            CHECK_FALSE(refusal.text.empty());
            CHECK_FALSE(refusal.text.contains('\n'));
        }
}

TEST_CASE("Notice texts", "[notices]")
{
    using namespace contour::window;
    CHECK(copiedNoticeText(1) == "Copied 1 line");
    CHECK(copiedNoticeText(3) == "Copied 3 lines");
    CHECK(pasteTooLargeNoticeText(std::size_t { 1024 } * 1024)
          == "Not pasted: the clipboard holds more than 1.0 MB of text.");
    CHECK(layoutSavedNoticeText("dev") == "Saved layout “dev”");
    CHECK(layoutSaveFailedNoticeText("dev", contour::config::LayoutSaveError::WriteFailed)
          == std::format("Layout “dev” was not saved: {}.",
                         contour::config::describe(contour::config::LayoutSaveError::WriteFailed)));
    CHECK(dropRefusedText(DropTextError::ControlCharacter)
          == "Not inserted: a file name holds a control or text-direction character.");
    CHECK(dropRefusedText(DropTextError::NoPaths).empty());
    CHECK(restoreSummaryText(2, 7) == "Restored 2 windows, 7 tabs");
    CHECK(restoreSummaryText(1, 1) == "Restored 1 window, 1 tab");
    CHECK(configReloadFailedNoticeText("bad key") == "Contour could not reload its configuration: bad key");

    // Names and errors are shown as text: no control reaches the window.
    CHECK_FALSE(layoutSavedNoticeText("dev\x1b]0;x\x07").contains('\x1b'));
    CHECK_FALSE(configReloadFailedNoticeText("line 1\nline 2\x1b[31m").contains('\x1b'));
    CHECK_FALSE(configReloadFailedNoticeText("line 1\nline 2").contains('\n'));
}

TEST_CASE("NoticeQueue: a toast shows for its row's duration", "[notices]")
{
    auto queue = NoticeQueue {};
    CHECK(queue.post(notice(NoticeKind::Copied, "Copied 3 lines"), T0) == NoticeChange::Changed);
    REQUIRE(queue.toast() != nullptr);
    CHECK(queue.toast()->text == "Copied 3 lines");
    CHECK(queue.nextDeadline() == T0 + 2000ms);
    CHECK(queue.expire(T0 + 1999ms) == NoticeChange::None);
    CHECK(queue.toast() != nullptr);
    CHECK(queue.expire(T0 + 2000ms) == NoticeChange::Changed);
    CHECK(queue.toast() == nullptr);
    CHECK_FALSE(queue.nextDeadline().has_value());
}

TEST_CASE("NoticeQueue: another kind waits its turn", "[notices]")
{
    auto queue = NoticeQueue {};
    std::ignore = queue.post(notice(NoticeKind::Copied, "Copied 1 line"), T0);
    CHECK(queue.post(notice(NoticeKind::PasteTooLarge, "Not pasted"), T0 + 100ms) == NoticeChange::None);
    CHECK(queue.toast()->kind == NoticeKind::Copied);

    auto const copiedDeadline = T0 + noticeRow(NoticeKind::Copied).duration;
    CHECK(queue.expire(copiedDeadline) == NoticeChange::Changed);
    REQUIRE(queue.toast() != nullptr);
    CHECK(queue.toast()->kind == NoticeKind::PasteTooLarge);
    CHECK(queue.nextDeadline() == copiedDeadline + 4000ms);
}

TEST_CASE("NoticeQueue: the same kind replaces the toast and restarts it", "[notices]")
{
    auto queue = NoticeQueue {};
    std::ignore = queue.post(notice(NoticeKind::Copied, "Copied 1 line"), T0);
    CHECK(queue.post(notice(NoticeKind::Copied, "Copied 2 lines"), T0 + 500ms) == NoticeChange::Changed);
    CHECK(queue.toast()->text == "Copied 2 lines");
    CHECK(queue.nextDeadline() == T0 + 500ms + 2000ms);
    CHECK(queue.expire(T0 + 500ms + 2000ms) == NoticeChange::Changed);
    CHECK(queue.toast() == nullptr); // one toast, not a backlog
}

TEST_CASE("NoticeQueue: each other kind waits once, in order", "[notices]")
{
    auto queue = NoticeQueue {};
    std::ignore = queue.post(notice(NoticeKind::Copied, "shown"), T0);
    auto const waiting = std::array {
        NoticeKind::PasteTooLarge,  NoticeKind::LayoutSaved,   NoticeKind::LayoutSaveFailed,
        NoticeKind::RestoreSummary, NoticeKind::PasteTooLarge, NoticeKind::LayoutSaved,
    };
    // Two of the six repeat a kind already waiting and replace it, in its place: four distinct wait.
    for (auto const index: std::views::iota(std::size_t { 0 }, waiting.size()))
        std::ignore = queue.post(notice(waiting[index], std::format("waiting {}", index)), T0);

    auto now = T0;
    auto shown = std::vector<std::string> {};
    while (auto const deadline = queue.nextDeadline())
    {
        now = *deadline;
        std::ignore = queue.expire(now);
        if (queue.toast() != nullptr)
            shown.push_back(queue.toast()->text);
    }
    CHECK(shown == std::vector<std::string> { "waiting 4", "waiting 5", "waiting 2", "waiting 3" });
}

TEST_CASE("NoticeQueue: a hovered toast stays", "[notices]")
{
    auto queue = NoticeQueue {};
    std::ignore = queue.post(notice(NoticeKind::Copied, "Copied 1 line"), T0);
    CHECK(queue.hover(ToastHover::Hovered, T0 + 100ms) == NoticeChange::None);
    CHECK_FALSE(queue.nextDeadline().has_value());
    CHECK(queue.expire(T0 + 1h) == NoticeChange::None);
    CHECK(queue.toast() != nullptr);

    auto const t1 = T0 + 1h;
    std::ignore = queue.hover(ToastHover::Away, t1);
    CHECK(queue.nextDeadline() == t1 + noticeRow(NoticeKind::Copied).duration);
}

TEST_CASE("NoticeQueue: one banner, replaced, cleared or dismissed", "[notices]")
{
    auto queue = NoticeQueue {};
    CHECK(queue.post(notice(NoticeKind::ConfigReloadFailed, "first"), T0) == NoticeChange::Changed);
    REQUIRE(queue.banner() != nullptr);
    CHECK(queue.banner()->text == "first");
    CHECK(queue.toast() == nullptr);
    CHECK_FALSE(queue.nextDeadline().has_value());

    std::ignore = queue.post(notice(NoticeKind::ConfigReloadFailed, "second"), T0 + 1s);
    CHECK(queue.banner()->text == "second");

    CHECK(queue.clear(NoticeKind::ConfigReloadFailed) == NoticeChange::Changed);
    CHECK(queue.banner() == nullptr);
    CHECK(queue.clear(NoticeKind::ConfigReloadFailed) == NoticeChange::None);

    std::ignore = queue.post(notice(NoticeKind::ConfigReloadFailed, "third"), T0 + 2s);
    CHECK(queue.dismissBanner() == NoticeChange::Changed);
    CHECK(queue.banner() == nullptr);
    CHECK(queue.dismissBanner() == NoticeChange::None);
}
