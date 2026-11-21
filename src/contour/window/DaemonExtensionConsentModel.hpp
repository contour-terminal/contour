// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a window shows of a daemon-scoped extension's question: the consent dialog's model.

#include <core/platform/Clock.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/PermissionWire.hpp>
#include <vthost/proto/Pdu.hpp>

namespace contour::window
{

/// How long Allow stays disabled in a window after the question is shown there, and after that window becomes
/// active, so that a click or a key aimed at what was there before -- or the click that activates the window
/// -- cannot grant what is shown. Deny and dismissing stay live.
inline constexpr auto DaemonConsentAllowHold = std::chrono::milliseconds { 500 };

/// What the windows of one daemon connection show of the daemon-scoped extensions' questions it was sent
/// (proto::ExtensionPermissionRequest): one at a time, newest first, each until it is answered, dismissed, or
/// settled by another window.
///
/// The question belongs to the connection; each window that shows it is a viewer with a hold of its own:
/// Allow is armed in a window only while that window is active, and only once DaemonConsentAllowHold has
/// passed since both the question was shown and the window became active. Return is Deny. GUI thread only.
class DaemonExtensionConsentModel: public QObject
{
    Q_OBJECT
    /// Whether a question is shown. A plain `bool`: QML reads it as a predicate (a documented exception to
    /// the enum-class rule for QML-facing properties).
    Q_PROPERTY(bool isVisible READ isVisible NOTIFY changed)
    /// The shown question's title.
    Q_PROPERTY(QString title READ title NOTIFY changed)
    /// The shown question's lines: plain text, never markup.
    Q_PROPERTY(QStringList lines READ lines NOTIFY changed)
    /// Counts every change, holds included: what a QML binding over isAllowHeldFor() depends on to be
    /// evaluated again.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

  public:
    /// Sends a window's answer to the daemon.
    using Reply = std::function<void(std::string name, std::string grantDigest, vthost::GrantState answer)>;
    /// Calls the job once the delay has passed, on the GUI thread, unless @p context is gone by then: what
    /// ends a hold. Production passes QTimer::singleShot.
    using Timer =
        std::function<void(QObject* context, std::chrono::milliseconds delay, std::function<void()> job)>;

    /// @param reply Where an answer goes.
    /// @param clock What a hold is timed on. Borrowed: it must outlive the model.
    /// @param timer What tells the model a hold is over, so that a dialog's Allow comes alive.
    /// @param parent The QObject parent.
    DaemonExtensionConsentModel(Reply reply,
                                core::platform::IClock const& clock,
                                Timer timer,
                                QObject* parent = nullptr);

    /// Shows @p request, ahead of any other: a new question, or one no longer shown, is shown -- held anew in
    /// every window -- and asks for focus. One already shown, or queued, changes nothing.
    /// @param request The question, as the daemon sent it.
    void show(vthost::proto::ExtensionPermissionRequest const& request);

    /// Closes a question another window answered; the next, if any, is shown. Others stay.
    /// @param name What the daemon calls the extension.
    /// @param grantDigest The question's digest.
    void settle(std::string_view name, std::string_view grantDigest);

    /// Drops every question: the connection they came with is gone. A reconnect is asked again what is open.
    void clear();

    /// A window that shows the dialog: its hold is its own. Starts inactive.
    /// @return The viewer's id, for the calls below.
    Q_INVOKABLE int attachViewer();
    /// The window no longer shows the dialog.
    Q_INVOKABLE void detachViewer(int viewer);
    /// The window became active, or stopped being active; becoming active holds Allow there anew.
    /// @param active Whether it is. A `bool` because QML hands over `Window.active` as one (a documented
    ///        exception for QML-facing calls).
    Q_INVOKABLE void setViewerActive(int viewer, bool active);
    /// @return Whether Allow is withheld in @p viewer's window now: it is not active, or became active, or
    /// was
    ///         shown the question, less than DaemonConsentAllowHold ago. True for an unknown viewer.
    Q_INVOKABLE [[nodiscard]] bool isAllowHeldFor(int viewer) const;

    /// Grants the shown question from @p viewer's window, unless Allow is held there; closes it.
    Q_INVOKABLE void allowFrom(int viewer);
    /// Refuses the shown question; closes it. What Return does.
    Q_INVOKABLE void deny();
    /// Closes the shown question without an answer: Escape and the close control. It is asked again when
    /// the daemon sends it again (`contour extension grant`, or the next attach).
    Q_INVOKABLE void dismiss();

    [[nodiscard]] bool isVisible() const noexcept { return !_questions.empty(); }
    [[nodiscard]] QString title() const;
    [[nodiscard]] QStringList lines() const;
    [[nodiscard]] int revision() const noexcept { return _revision; }

  signals:
    /// What the dialog shows changed, or a hold began or ended.
    void changed();
    /// A question was put up: the dialog opens and takes the focus. Not emitted for a re-sent one.
    void focusRequested();

  private:
    /// Whether a viewer's window is active.
    enum class ViewerFocus : uint8_t
    {
        Inactive = 0,
        Active,
    };

    /// One window showing the dialog.
    struct Viewer
    {
        ViewerFocus focus = ViewerFocus::Inactive;
        core::platform::SteadyTimePoint activeSince {}; ///< When it last became active.
    };

    /// Closes the shown question; shows the next, if any.
    void popShown();
    /// Starts the hold of the question now shown, and asks for focus.
    void presentFront();
    /// Emits changed() now, and again once a hold begun now has passed.
    void holdBegan();
    /// Emits changed(), counting it.
    void notifyChanged();

    Reply _reply;
    core::platform::IClock const& _clock;
    Timer _timer;
    std::vector<vthost::proto::ExtensionPermissionRequest> _questions; ///< Front is shown.
    core::platform::SteadyTimePoint _shownAt {};                       ///< When the front was shown.
    std::map<int, Viewer> _viewers;
    int _nextViewer = 1;
    int _revision = 0;
};

} // namespace contour::window
