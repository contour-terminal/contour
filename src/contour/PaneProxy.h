// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/TerminalSession.h>

#include <QtCore/QObject>
#include <QtQml/QtQml>

#include <vtworkspace/Pane.h>
#include <vtworkspace/Primitives.h>

namespace contour
{

class TerminalSessionManager;

/// A thin, observable QML wrapper around one node of a tab's vtworkspace::Pane tree.
///
/// The recursive split layout in QML (PaneNode.qml) reads these proxies: a leaf proxy exposes the
/// TerminalSession to render, while a split proxy exposes its orientation, ratio and two child
/// proxies. The proxy holds no authority — it forwards reads to the underlying vtworkspace::Pane and
/// writes (the splitter ratio) back through the manager so the model stays the single source of
/// truth. Proxies are created and owned by the manager, keyed by PaneId, and reused across rebinds
/// so a surviving pane's ContourTerminal is not torn down when its sibling splits or closes.
class PaneProxy: public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isLeaf READ isLeaf NOTIFY changed)
    Q_PROPERTY(int orientation READ orientation NOTIFY changed) // vtworkspace::SplitState as int
    Q_PROPERTY(double ratio READ ratio NOTIFY ratioChanged)
    Q_PROPERTY(contour::PaneProxy* first READ first NOTIFY changed)
    Q_PROPERTY(contour::PaneProxy* second READ second NOTIFY changed)
    Q_PROPERTY(contour::TerminalSession* session READ session NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(int paneId READ paneId CONSTANT)
    QML_ELEMENT
    QML_UNCREATABLE("Created by the session manager")

  public:
    PaneProxy(TerminalSessionManager& manager, vtworkspace::PaneId id) noexcept:
        _manager { manager }, _id { id }
    {
    }

    [[nodiscard]] int paneId() const noexcept { return static_cast<int>(_id.value); }

    [[nodiscard]] bool isLeaf() const noexcept;
    [[nodiscard]] int orientation() const noexcept;
    [[nodiscard]] double ratio() const noexcept;
    [[nodiscard]] PaneProxy* first() const noexcept { return _first; }
    [[nodiscard]] PaneProxy* second() const noexcept { return _second; }
    [[nodiscard]] TerminalSession* session() const noexcept;
    [[nodiscard]] bool active() const noexcept;

    /// Writes a new splitter ratio back to the model (called from the QML drag handle).
    Q_INVOKABLE void setRatio(double ratio);

    /// Makes this pane the active pane of its tab (called on focus-in / click).
    Q_INVOKABLE void activate();

    // {{{ Manager-side wiring (not for QML)
    [[nodiscard]] vtworkspace::PaneId modelId() const noexcept { return _id; }
    void setChildren(PaneProxy* first, PaneProxy* second) noexcept
    {
        _first = first;
        _second = second;
    }

    /// Caches the backing vtworkspace::Pane this proxy resolves to — so the property getters need not walk
    /// the tab tree on every QML binding evaluation — together with the HOSTING TAB's id, which keys
    /// every write-back (setRatio/activate) so a proxy of any window's tab targets exactly that tab.
    /// The controller re-points this from rebuildActiveTabPaneProxies() (which already walks the live
    /// tree) on each structural change, and clears it (nullptr pane, default tab) when the pane is
    /// pruned. @p pane may be null.
    void setPane(vtworkspace::Pane* pane, vtworkspace::TabId tab) noexcept
    {
        _pane = pane;
        _tab = tab;
    }

    void notifyChanged() { emit changed(); }
    void notifyRatioChanged() { emit ratioChanged(); }
    void notifyActiveChanged() { emit activeChanged(); }
    // }}}

  signals:
    void changed();
    void ratioChanged();
    void activeChanged();

  private:
    /// The backing pane, cached by the manager (see setPane). Null while the pane does not exist in
    /// the active tab (e.g. just-pruned, or this proxy is not part of the active tab's tree).
    [[nodiscard]] vtworkspace::Pane* pane() const noexcept { return _pane; }

    TerminalSessionManager& _manager;
    vtworkspace::PaneId _id;
    /// The tab hosting this pane (cached alongside _pane); keys the manager write-backs.
    vtworkspace::TabId _tab {};
    vtworkspace::Pane* _pane = nullptr;
    PaneProxy* _first = nullptr;
    PaneProxy* _second = nullptr;
};

} // namespace contour
