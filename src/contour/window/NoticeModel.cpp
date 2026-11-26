// SPDX-License-Identifier: Apache-2.0
#include <contour/window/NoticeModel.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace contour::window
{

namespace
{
    [[nodiscard]] QString textOf(Notice const* notice)
    {
        return notice != nullptr ? QString::fromStdString(notice->text) : QString {};
    }

    [[nodiscard]] QString glyphOf(Notice const* notice)
    {
        return notice != nullptr ? QString::fromUtf8(noticeRow(notice->kind).glyph) : QString {};
    }

    [[nodiscard]] std::optional<Notice> copyOf(Notice const* notice)
    {
        return notice != nullptr ? std::optional { *notice } : std::nullopt;
    }
} // namespace

NoticeModel::NoticeModel(core::platform::IClock const& clock,
                         platform::Announcer& announcer,
                         ActionRunner onAction,
                         QObject* parent):
    QObject(parent), _clock { clock }, _announcer { announcer }, _onAction { std::move(onAction) }
{
    _expiry.setSingleShot(true);
    connect(&_expiry, &QTimer::timeout, this, &NoticeModel::expireDue);
}

QString NoticeModel::toastText() const
{
    return textOf(_queue.toast());
}

QString NoticeModel::toastGlyph() const
{
    return glyphOf(_queue.toast());
}

QString NoticeModel::toastActionText() const
{
    auto const* const toast = _queue.toast();
    return toast != nullptr ? QString::fromUtf8(noticeRow(toast->kind).actionText) : QString {};
}

QString NoticeModel::bannerText() const
{
    return textOf(_queue.banner());
}

QString NoticeModel::bannerGlyph() const
{
    return glyphOf(_queue.banner());
}

QString NoticeModel::bannerActionText() const
{
    auto const* const banner = _queue.banner();
    return banner != nullptr ? QString::fromUtf8(noticeRow(banner->kind).actionText) : QString {};
}

void NoticeModel::post(Notice notice)
{
    settle(_queue.post(std::move(notice), _clock.now()));
}

void NoticeModel::clear(NoticeKind kind)
{
    settle(_queue.clear(kind));
}

void NoticeModel::setToastHovered(bool hovered)
{
    settle(_queue.hover(hovered ? ToastHover::Hovered : ToastHover::Away, _clock.now()));
}

void NoticeModel::setToastFocused(bool focused)
{
    settle(_queue.focus(focused ? ToastFocus::Focused : ToastFocus::Away, _clock.now()));
}

void NoticeModel::dismissBanner()
{
    settle(_queue.dismissBanner());
}

void NoticeModel::triggerBannerAction()
{
    auto const* const banner = _queue.banner();
    if (banner == nullptr)
        return;
    if (auto const action = noticeRow(banner->kind).action; action != NoticeAction::None && _onAction)
        _onAction(action, banner->subject);
}

void NoticeModel::triggerToastAction()
{
    auto const* const toast = _queue.toast();
    if (toast == nullptr)
        return;
    auto const action = noticeRow(toast->kind).action;
    if (action == NoticeAction::None)
        return;
    // Copied: the action may post a notice, and ending the toast below drops this one.
    auto const shown = *toast;
    if (_onAction)
        _onAction(action, shown.subject);
    settle(_queue.clear(shown.kind));
}

void NoticeModel::expireDue()
{
    settle(_queue.expire(_clock.now()));
}

void NoticeModel::settle(NoticeChange change)
{
    if (change == NoticeChange::Changed)
    {
        auto toast = copyOf(_queue.toast());
        auto banner = copyOf(_queue.banner());
        if (toast != _shownToast)
        {
            _shownToast = std::move(toast);
            emit toastChanged();
            if (_shownToast)
                _announcer.announce(QString::fromStdString(_shownToast->text));
        }
        if (banner != _shownBanner)
        {
            _shownBanner = std::move(banner);
            emit bannerChanged();
            if (_shownBanner)
                _announcer.announce(QString::fromStdString(_shownBanner->text));
        }
    }

    // Armed for the toast shown, unless it is hovered; a hovered toast is rearmed as the pointer leaves.
    if (auto const deadline = _queue.nextDeadline())
    {
        auto const remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - _clock.now());
        _expiry.start(std::max(remaining, std::chrono::milliseconds { 0 }));
    }
    else
        _expiry.stop();
}

} // namespace contour::window
