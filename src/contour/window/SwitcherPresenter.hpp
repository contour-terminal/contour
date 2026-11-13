// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/SwitcherRows.hpp>

#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QRectF>

#include <functional>
#include <optional>
#include <vector>

namespace contour::window
{

class SidebarModel;
class SwitcherController;

/// A window the switcher popup is presented in: WindowController, or a test's stand-in.
class SwitcherHost
{
  public:
    SwitcherHost() = default;
    SwitcherHost(SwitcherHost const&) = delete;
    SwitcherHost& operator=(SwitcherHost const&) = delete;
    SwitcherHost(SwitcherHost&&) = delete;
    SwitcherHost& operator=(SwitcherHost&&) = delete;

    /// Presents @p switcher in the window's scene: centred, or hanging below @p anchor.
    /// @param switcher The switcher, opened already; borrowed.
    /// @param anchor What it hangs from, in the window's coordinates; nullopt centres it.
    virtual void presentSwitcher(SwitcherController* switcher, std::optional<QRectF> anchor) = 0;

    /// Closes the switcher the window presents, if any, and lets go of it at once; the popup keeps drawing
    /// what it showed until its exit transition ends.
    virtual void withdrawSwitcher() = 0;

    /// @return The QObject that is the window: the switcher lets go of it as it is destroyed.
    [[nodiscard]] virtual QObject& switcherHostObject() noexcept = 0;

  protected:
    ~SwitcherHost() = default;
};

/// The application's one switcher, moved between the windows it is opened over: it is presented in one window
/// at a time, withdrawn from it once a choice is made or the user gives up, and abandoned (whatever it
/// started is no longer wanted) once that window goes.
class SwitcherPresenter final: public QObject
{
    Q_OBJECT

  public:
    /// @param switcher The switcher it presents; borrowed, it outlives the presenter.
    /// @param parent Qt ownership.
    explicit SwitcherPresenter(SwitcherController& switcher, QObject* parent = nullptr);

    /// Opens the switcher afresh with @p opening and presents it over @p host: withdrawn from the window it
    /// was presented in before, if that is another one.
    /// @param host The window it is opened over.
    /// @param opening What it shows.
    /// @param anchor What it hangs from, in @p host's coordinates; nullopt centres it.
    void open(SwitcherHost& host, SwitcherOpening opening, std::optional<QRectF> anchor);

    /// Keeps the switcher's own workspaces in step with @p sidebar while it stays presented where it was
    /// opened last: each rebuild of the sidebar hands it what @p windowWorkspaces reads then (status changes,
    /// the selection stays). Ends with that presentation: withdrawn, the window gone, or opened again.
    /// @param sidebar The sidebar of the window the switcher was opened over; outlives the presentation.
    /// @param windowWorkspaces Reads that window's own workspaces, with their status.
    void followSidebar(SidebarModel const& sidebar,
                       std::function<std::vector<SwitcherWorkspace>()> windowWorkspaces);

    /// @return The window the switcher is presented in; null for none.
    [[nodiscard]] SwitcherHost* host() const noexcept;

  private:
    /// Withdraws the switcher from its window: a choice was made, or the user gave up.
    void withdraw();

    /// Lets go of the window, which is being destroyed: what the switcher started is abandoned.
    void hostDestroyed();

    SwitcherController& _switcher;
    SwitcherHost* _host = nullptr; ///< Valid while _hostObject is.
    QPointer<QObject> _hostObject; ///< _host's QObject: null once it is gone.
    QMetaObject::Connection _hostGone;
    QMetaObject::Connection _sidebarFollowed; ///< followSidebar()'s; live only while presented.
};

} // namespace contour::window
