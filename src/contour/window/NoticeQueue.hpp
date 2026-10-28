// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/LayoutStore.hpp>
#include <contour/platform/ShellRcEditor.hpp>
#include <contour/session/PastePreview.hpp>
#include <contour/window/DroppedText.hpp>

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace contour::window
{

/// What a window tells its user without asking anything back.
enum class NoticeKind : uint8_t
{
    Copied = 0,             ///< An explicit copy put text on the clipboard.
    PasteTooLarge,          ///< A paste was refused for its size.
    LayoutSaved,            ///< Save Layout wrote the layout.
    LayoutSaveFailed,       ///< Save Layout could not.
    RestoreSummary,         ///< What a restore brought back.
    ConfigReloadFailed,     ///< The configuration file could not be loaded.
    DropRefused,            ///< A drop onto a pane was refused, and nothing was typed.
    ShellIntegrationAdded,  ///< Install Shell Integration added its line to the shell's startup file.
    ShellIntegrationFailed, ///< It could not.
};

/// Where a notice is shown.
enum class NoticeSurface : uint8_t
{
    Toast = 0, ///< A short-lived pill, one at a time.
    Banner,    ///< A row under the title band, until cleared or dismissed.
};

/// What a notice's action button does.
enum class NoticeAction : uint8_t
{
    None = 0,       ///< It has none.
    OpenConfigFile, ///< Opens the configuration file in the user's editor.
};

/// How a kind of notice is shown, and for how long.
struct NoticeRow
{
    NoticeKind kind;
    NoticeSurface surface;
    std::string_view glyph;             ///< The mark before its text; empty for none.
    std::chrono::milliseconds duration; ///< How long a toast shows once current; zero: until cleared.
    NoticeAction action;
    std::string_view actionText; ///< The action button's label; empty without an action.
};

/// One row per notice kind, in enumerator order.
inline constexpr auto NoticeRows = std::array {
    NoticeRow { .kind = NoticeKind::Copied,
                .surface = NoticeSurface::Toast,
                .glyph = "",
                .duration = std::chrono::milliseconds { 2000 },
                .action = NoticeAction::None,
                .actionText = "" },
    NoticeRow { .kind = NoticeKind::PasteTooLarge,
                .surface = NoticeSurface::Toast,
                .glyph = "!",
                .duration = std::chrono::milliseconds { 4000 },
                .action = NoticeAction::None,
                .actionText = "" },
    NoticeRow { .kind = NoticeKind::LayoutSaved,
                .surface = NoticeSurface::Toast,
                .glyph = "",
                .duration = std::chrono::milliseconds { 2000 },
                .action = NoticeAction::None,
                .actionText = "" },
    NoticeRow { .kind = NoticeKind::LayoutSaveFailed,
                .surface = NoticeSurface::Toast,
                .glyph = "!",
                .duration = std::chrono::milliseconds { 6000 },
                .action = NoticeAction::None,
                .actionText = "" },
    NoticeRow { .kind = NoticeKind::RestoreSummary,
                .surface = NoticeSurface::Toast,
                .glyph = "",
                .duration = std::chrono::milliseconds { 4000 },
                .action = NoticeAction::None,
                .actionText = "" },
    NoticeRow { .kind = NoticeKind::ConfigReloadFailed,
                .surface = NoticeSurface::Banner,
                .glyph = "!",
                .duration = std::chrono::milliseconds { 0 },
                .action = NoticeAction::OpenConfigFile,
                .actionText = "Show in Editor" },
    NoticeRow { .kind = NoticeKind::DropRefused,
                .surface = NoticeSurface::Toast,
                .glyph = "!",
                .duration = std::chrono::milliseconds { 4000 },
                .action = NoticeAction::None,
                .actionText = "" },
    NoticeRow { .kind = NoticeKind::ShellIntegrationAdded,
                .surface = NoticeSurface::Toast,
                .glyph = "",
                .duration = std::chrono::milliseconds { 4000 },
                .action = NoticeAction::None,
                .actionText = "" },
    NoticeRow { .kind = NoticeKind::ShellIntegrationFailed,
                .surface = NoticeSurface::Toast,
                .glyph = "!",
                .duration = std::chrono::milliseconds { 6000 },
                .action = NoticeAction::None,
                .actionText = "" },
};

static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, NoticeRows.size()))
            if (std::to_underlying(NoticeRows[index].kind) != index)
                return false;
        return true;
    }(),
    "NoticeRows must list every notice kind in enumerator order");
static_assert(NoticeRows.size() == std::to_underlying(NoticeKind::ShellIntegrationFailed) + 1U,
              "NoticeRows must have a row for every notice kind");

/// The row of @p kind.
[[nodiscard]] constexpr NoticeRow const& noticeRow(NoticeKind kind) noexcept
{
    return NoticeRows[std::to_underlying(kind)];
}

/// One notice: its kind, which decides how it shows, and what it says.
struct Notice
{
    NoticeKind kind = NoticeKind::Copied;
    std::string text {}; ///< Plain text, already made safe to show.

    bool operator==(Notice const&) const = default;
};

namespace detail
{
    /// @p text made safe to show on one row: line breaks become spaces, other controls U+FFFD.
    [[nodiscard]] inline std::string shownText(std::string_view text)
    {
        auto oneRow = std::string { text };
        std::ranges::replace(oneRow, '\n', ' ');
        std::ranges::replace(oneRow, '\r', ' ');
        return vtbackend::sanitizeCommandLine(oneRow, vtbackend::SanitizePurpose::Display);
    }

    /// @p count followed by @p noun, in the plural unless it is one: "1 line", "3 lines".
    [[nodiscard]] inline std::string counted(std::size_t count, std::string_view noun)
    {
        return std::format("{} {}{}", count, noun, count == 1 ? "" : "s");
    }
} // namespace detail

/// "Copied 3 lines".
/// @param lines How many lines were copied.
[[nodiscard]] inline std::string copiedNoticeText(std::size_t lines)
{
    return std::format("Copied {}", detail::counted(lines, "line"));
}

/// "Not pasted: the clipboard holds more than 1.0 MB of text."
/// @param limitBytes The most a paste may hold.
[[nodiscard]] inline std::string pasteTooLargeNoticeText(std::size_t limitBytes)
{
    return std::format("Not pasted: the clipboard holds more than {} of text.",
                       session::formatByteCount(limitBytes));
}

/// "Saved layout “dev”".
/// @param name The layout's name, as typed.
[[nodiscard]] inline std::string layoutSavedNoticeText(std::string_view name)
{
    return std::format("Saved layout “{}”", detail::shownText(name));
}

/// "Layout “dev” was not saved: the layouts file could not be written."
/// @param name The layout's name, as typed.
/// @param error Why not.
[[nodiscard]] inline std::string layoutSaveFailedNoticeText(std::string_view name,
                                                            config::LayoutSaveError error)
{
    return std::format("Layout “{}” was not saved: {}.",
                       detail::shownText(name),
                       detail::shownText(config::describe(error)));
}

/// "Restored 2 windows, 7 tabs".
/// @param windows How many windows a restore brought back.
/// @param tabs How many tabs.
[[nodiscard]] inline std::string restoreSummaryText(std::size_t windows, std::size_t tabs)
{
    return std::format("Restored {}, {}", detail::counted(windows, "window"), detail::counted(tabs, "tab"));
}

/// "Contour could not reload its configuration: {error}".
/// @param error What the loader said; YAML errors quote the file, so it is made safe to show.
[[nodiscard]] inline std::string configReloadFailedNoticeText(std::string_view error)
{
    return std::format("Contour could not reload its configuration: {}", detail::shownText(error));
}

/// What a refused drop held, which is what its toast calls the offender.
enum class DropSubject : uint8_t
{
    FileName = 0, //!< Every dropped item was a local file.
    Link,         //!< Every dropped item was a link that is not a local file.
    NameOrLink,   //!< Files and links were dropped together.
};

/// How the toast names each @c DropSubject, in enumerator order.
inline constexpr auto DropSubjectNouns =
    std::array<std::string_view, 3> { "a file name", "a link", "a name" };

static_assert(DropSubjectNouns.size() == std::to_underlying(DropSubject::NameOrLink) + 1U,
              "DropSubjectNouns must have a noun for every drop subject");

/// A row of the drop-refusal table: why dropped paths gave no text, and what the toast says.
struct DropRefusalRow
{
    DropTextError error; ///< Why nothing was typed.
    std::string_view
        text; ///< What the toast says, `{}` standing for the subject's noun; empty where nothing is shown.
};

/// One row per @c DropTextError, in enumerator order. "No paths" is no refusal worth a toast: there was
/// nothing to type.
inline constexpr auto DropRefusalRows = std::array {
    DropRefusalRow { .error = DropTextError::NoPaths, .text = "" },
    DropRefusalRow { .error = DropTextError::ControlCharacter,
                     .text = "Not inserted: {} holds a control or text-direction character." },
    DropRefusalRow { .error = DropTextError::Unquotable,
                     .text = "Not inserted: {} holds a character cmd cannot quote." },
    DropRefusalRow { .error = DropTextError::UnknownShell,
                     .text = "Not inserted: {} holds a character that could run as a command, and "
                             "this pane's shell is not known." },
    DropRefusalRow { .error = DropTextError::LeadingDash,
                     .text = "Not inserted: {} starts with a dash, which a program would read as an "
                             "option." },
};

static_assert(DropRefusalRows.size() == std::to_underlying(DropTextError::LeadingDash) + 1U,
              "DropRefusalRows must have a row for every drop error");

/// What the toast says when text holding a line break is dropped on a pane that has not enabled bracketed
/// paste: typed, each line would run as it arrives.
inline constexpr std::string_view DropTextLineBreakText =
    "Not inserted: the text has a line break and this program has not asked for bracketed paste.";

/// What the toast says when a tab dropped on a pane of a daemon-hosted window cannot be merged: the daemon
/// owns the layout, and it did not take the request (or no longer can).
inline constexpr std::string_view TabMergeRefusedText =
    "Not merged: the daemon could not take the tab into that pane.";

/// What the toast for a refused drop says.
/// @param error Why the drop was refused.
/// @param subject What the drop held, named in the text.
/// @return The text; empty when @p error needs no toast.
[[nodiscard]] inline std::string dropRefusedText(DropTextError error, DropSubject subject)
{
    auto const text = DropRefusalRows[std::to_underlying(error)].text;
    return std::vformat(text, std::make_format_args(DropSubjectNouns[std::to_underlying(subject)]));
}

/// What the toast says of a successful install, by platform::RcInstall: `{0}` is the startup file as the
/// user would say it, `{1}` the shell's name.
inline constexpr auto ShellIntegrationInstalledPatterns = std::array<std::string_view, 2> {
    "Added shell integration to {0}. New {1} sessions show command blocks.",
    "Shell integration was already in {0}. New {1} sessions show command blocks.",
};
static_assert(ShellIntegrationInstalledPatterns.size()
              == std::to_underlying(platform::RcInstall::AlreadyInstalled) + 1U);

/// "Added shell integration to ~/.zshrc. New zsh sessions show command blocks.", or, when the file
/// already loaded it, "Shell integration was already in ~/.zshrc. …".
/// @param result What the install did.
/// @param rc The startup file as the user would say it.
/// @param shell The shell's name.
[[nodiscard]] inline std::string shellIntegrationInstalledText(platform::RcInstall result,
                                                               std::string_view rc,
                                                               std::string_view shell)
{
    auto const shownRc = detail::shownText(rc);
    auto const shownShell = detail::shownText(shell);
    return std::vformat(ShellIntegrationInstalledPatterns[std::to_underlying(result)],
                        std::make_format_args(shownRc, shownShell));
}

/// "~/.zshrc was not changed: the file could not be written.", or, for an error that may have changed the
/// file (platform::changedFile), "~/.zshrc now ends in a cut-short Contour line: …".
/// @param rc The startup file as the user would say it.
/// @param error Why not.
[[nodiscard]] inline std::string shellIntegrationFailedText(std::string_view rc, platform::ShellRcError error)
{
    if (platform::changedFile(error))
        return std::format("{} {}.", detail::shownText(rc), platform::describe(error));
    return std::format("{} was not changed: {}.", detail::shownText(rc), platform::describe(error));
}

/// Whether the pointer rests on the toast.
enum class ToastHover : uint8_t
{
    Away = 0, ///< Not on it: its time runs.
    Hovered,  ///< On it: its time stands still.
};

/// Whether a call changed what is shown.
enum class NoticeChange : uint8_t
{
    None = 0, ///< The toast and the banner are what they were.
    Changed,  ///< The toast or the banner changed.
};

/// How many notice kinds show as a toast.
inline constexpr auto ToastKindCount =
    static_cast<std::size_t>(std::ranges::count(NoticeRows, NoticeSurface::Toast, &NoticeRow::surface));

/// The most toasts that wait behind the one shown. No drop is needed to keep to it: a waiting toast of a kind
/// already waiting replaces it, and one of the shown kind replaces the shown one, so at most one per other
/// toast kind waits. Derived from the table, so a new toast kind raises it rather than breaking the build.
inline constexpr std::size_t NoticeWaitingLimit = ToastKindCount - 1;

/// A window's notices: one toast shown at a time, others waiting, and one banner.
///
/// A toast shows for its row's duration from the moment it becomes current; while hovered its deadline is
/// suspended, and leaving restarts the full duration. A toast posted while one of the same kind is current
/// replaces its text and restarts it; one posted while another kind shows waits, replacing a waiting one of
/// its own kind, so at most one of each other kind waits (NoticeWaitingLimit). The banner slot holds one
/// notice; a later post replaces it. Time comes in through each call, so the queue is a value a test steps
/// through.
class NoticeQueue
{
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    /// Shows @p notice, or queues it behind the toast shown.
    /// @return Changed when the toast or banner shown changed.
    [[nodiscard]] NoticeChange post(Notice notice, TimePoint now)
    {
        _lastNow = now;
        if (noticeRow(notice.kind).surface == NoticeSurface::Banner)
        {
            _banner = std::move(notice);
            return NoticeChange::Changed;
        }
        if (!_toast || _toast->kind == notice.kind)
        {
            _toast = std::move(notice);
            restartDeadline(now);
            return NoticeChange::Changed;
        }
        if (auto const same = std::ranges::find(_waiting, notice.kind, &Notice::kind); same != _waiting.end())
            *same = std::move(notice);
        else
            _waiting.push_back(std::move(notice));
        return NoticeChange::None;
    }

    /// Takes every notice of @p kind away, shown or waiting.
    /// @return Changed when the toast or banner shown changed.
    [[nodiscard]] NoticeChange clear(NoticeKind kind)
    {
        auto change = NoticeChange::None;
        if (_banner && _banner->kind == kind)
        {
            _banner.reset();
            change = NoticeChange::Changed;
        }
        std::erase_if(_waiting, [kind](Notice const& notice) { return notice.kind == kind; });
        if (_toast && _toast->kind == kind)
        {
            // The next one starts from the last moment this queue was told of.
            promoteNext(_lastNow);
            change = NoticeChange::Changed;
        }
        return change;
    }

    /// The pointer came onto the toast, or left it.
    /// @return None: hovering changes when a toast goes, not what shows.
    [[nodiscard]] NoticeChange hover(ToastHover hover, TimePoint now)
    {
        _lastNow = now;
        _hover = hover;
        if (_hover == ToastHover::Away && _toast)
            restartDeadline(now);
        return NoticeChange::None;
    }

    /// Takes the toast away if its time is up, and shows the next one waiting.
    /// @return Changed when it did.
    [[nodiscard]] NoticeChange expire(TimePoint now)
    {
        _lastNow = now;
        auto const deadline = nextDeadline();
        if (!deadline || now < *deadline)
            return NoticeChange::None;
        promoteNext(now);
        return NoticeChange::Changed;
    }

    /// The user closed the banner.
    /// @return Changed when there was one.
    [[nodiscard]] NoticeChange dismissBanner()
    {
        if (!_banner)
            return NoticeChange::None;
        _banner.reset();
        return NoticeChange::Changed;
    }

    /// @return The toast shown; null for none.
    [[nodiscard]] Notice const* toast() const noexcept { return _toast ? &*_toast : nullptr; }

    /// @return The banner shown; null for none.
    [[nodiscard]] Notice const* banner() const noexcept { return _banner ? &*_banner : nullptr; }

    /// @return When the toast shown goes; nothing while none shows or it is hovered.
    [[nodiscard]] std::optional<TimePoint> nextDeadline() const noexcept
    {
        if (!_toast || _hover == ToastHover::Hovered)
            return std::nullopt;
        return _deadline;
    }

  private:
    void restartDeadline(TimePoint now) { _deadline = now + noticeRow(_toast->kind).duration; }

    void promoteNext(TimePoint now)
    {
        _toast.reset();
        if (_waiting.empty())
            return;
        _toast = std::move(_waiting.front());
        _waiting.pop_front();
        restartDeadline(now);
    }

    std::optional<Notice> _toast;
    std::deque<Notice> _waiting;
    std::optional<Notice> _banner;
    TimePoint _deadline {};
    TimePoint _lastNow {};
    ToastHover _hover = ToastHover::Away;
};

} // namespace contour::window
