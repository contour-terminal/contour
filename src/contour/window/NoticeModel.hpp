// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/Announcer.hpp>
#include <contour/window/NoticeQueue.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <functional>
#include <optional>

namespace contour::window
{

/// A window's notices, for QML: the toast and the banner its NoticeQueue shows, the timer that ends a toast,
/// and the announcement that tells a screen reader. GUI thread.
class NoticeModel final: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString toastText READ toastText NOTIFY toastChanged)
    Q_PROPERTY(QString toastGlyph READ toastGlyph NOTIFY toastChanged)
    // A bool for QML, the documented carve-out: whether a toast shows.
    Q_PROPERTY(bool toastShown READ toastShown NOTIFY toastChanged)
    Q_PROPERTY(QString bannerText READ bannerText NOTIFY bannerChanged)
    Q_PROPERTY(QString bannerGlyph READ bannerGlyph NOTIFY bannerChanged)
    Q_PROPERTY(QString bannerActionText READ bannerActionText NOTIFY bannerChanged)

  public:
    /// @param clock What a toast's time is read from.
    /// @param announcer What tells a screen reader of a toast or banner; must outlive the model.
    /// @param onAction Runs a banner's action when the user takes it.
    /// @param parent The QObject parent.
    NoticeModel(core::platform::IClock const& clock,
                platform::Announcer& announcer,
                std::function<void(NoticeAction)> onAction,
                QObject* parent = nullptr);

    /// @return The toast's text; empty while none shows.
    [[nodiscard]] QString toastText() const;
    /// @return The mark before the toast's text; empty for none.
    [[nodiscard]] QString toastGlyph() const;
    /// @return Whether a toast shows.
    [[nodiscard]] bool toastShown() const noexcept { return _queue.toast() != nullptr; }
    /// @return The banner's text; empty while none shows.
    [[nodiscard]] QString bannerText() const;
    /// @return The mark before the banner's text; empty for none.
    [[nodiscard]] QString bannerGlyph() const;
    /// @return The banner's action button label; empty without one.
    [[nodiscard]] QString bannerActionText() const;

    /// Shows @p notice, or queues it behind the toast shown (see NoticeQueue). GUI thread, and never under a
    /// terminal's lock: a toast that shows asks the window where to go, which reads the cursor under it.
    void post(Notice notice);

    /// Takes every notice of @p kind away.
    void clear(NoticeKind kind);

    /// The pointer came onto the toast, or left it: a hovered toast stays.
    /// @param hovered Whether it is on it -- a bool for QML (HoverHandler.hovered), the documented carve-out.
    Q_INVOKABLE void setToastHovered(bool hovered);

    /// The user closed the banner.
    Q_INVOKABLE void dismissBanner();

    /// The user took the banner's action.
    Q_INVOKABLE void triggerBannerAction();

    /// Ends the toast if its time is up: what the model's timer runs, and what tests call after advancing
    /// their clock.
    void expireDue();

  signals:
    void toastChanged();
    void bannerChanged();

  private:
    /// Emits what @p change changed, announces what newly shows, and rearms the timer.
    void settle(NoticeChange change);

    core::platform::IClock const& _clock;
    platform::Announcer& _announcer;
    std::function<void(NoticeAction)> _onAction;
    NoticeQueue _queue;
    QTimer _expiry;
    std::optional<Notice> _shownToast;  ///< The toast last published, to tell what changed.
    std::optional<Notice> _shownBanner; ///< The banner last published.
};

} // namespace contour::window
