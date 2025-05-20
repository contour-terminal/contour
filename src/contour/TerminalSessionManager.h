#pragma once

#include <contour/TerminalSession.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/helper.h>

#include <QtCore/QAbstractListModel>
#include <QtQml/QQmlEngine>

#include <unordered_map>
#include <vector>

namespace contour
{

/**
 * Manages terminal sessions.
 */
class TerminalSessionManager: public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count)
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QML_ELEMENT
#endif

  public:
    TerminalSessionManager(ContourGuiApp& app);

    contour::TerminalSession* createSessionInBackground();

    Q_INVOKABLE contour::TerminalSession* createSession();

    void switchToPreviousTab();
    void switchToTabLeft();
    void switchToTabRight();
    void switchToTab(int position);
    Q_INVOKABLE void closeTab();
    void moveTabTo(int position);
    void moveTabToLeft(TerminalSession* session);
    void moveTabToRight(TerminalSession* session);

    void setSession(size_t index);

    void removeSession(TerminalSession&);
    void currentSessionIsTerminated();

    Q_INVOKABLE [[nodiscard]] QVariant data(const QModelIndex& index,
                                            int role = Qt::DisplayRole) const override;
    Q_INVOKABLE [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(_sessions.size()); }

    void updateColorPreference(vtbackend::ColorPreference const& preference);

    display::TerminalDisplay* activeDisplay = nullptr;
    void FocusOnDisplay(display::TerminalDisplay* display);

    void update() { updateStatusLine(); }

    void allowCreation() { _allowCreation = true; }

  private:
    contour::TerminalSession* activateSession(TerminalSession* session, bool isNewSession = false);
    std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string> cwd);

    void tryFindSessionForDisplayOrClose();

    [[nodiscard]] std::optional<std::size_t> getSessionIndexOf(TerminalSession* session) const noexcept
    {
        if (auto const i = std::ranges::find(_sessions, session); i != _sessions.end())
            return static_cast<std::size_t>(std::distance(_sessions.begin(), i));
        return std::nullopt;
    }

    [[nodiscard]] auto getCurrentSessionIndex() noexcept
    {
        // TODO cache this value
        return getSessionIndexOf(_displayStates[activeDisplay]).value();
    }

    void updateStatusLine()
    {
        if (!_displayStates[activeDisplay])
            return;
        _displayStates[activeDisplay]->terminal().setGuiTabInfoForStatusLine(vtbackend::TabsInfo {
            .tabs = std::ranges::transform_view(_sessions,
                                                [](auto* session) {
                                                    return vtbackend::TabsInfo::Tab {
                                                        .name = session->name(),
                                                        .color = vtbackend::RGBColor { 0, 0, 0 },
                                                    };
                                                })
                    | ranges::to<std::vector>(),
            .activeTabPosition = 1 + getSessionIndexOf(_displayStates[activeDisplay]).value_or(0),
        });
    }

    ContourGuiApp& _app;
    std::chrono::seconds _earlyExitThreshold;
    std::unordered_map<display::TerminalDisplay*, TerminalSession*> _displayStates;
    std::vector<TerminalSession*> _sessions;

    // on windows qt tries to create a new session
    // twice on qml file loading, this bool is used to
    // prevent that, and to allow creation of new session
    // user have to call allowCreation() method first
    std::atomic<bool> _allowCreation;
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSessionManager, "org.contour.TerminalSessionManager")
