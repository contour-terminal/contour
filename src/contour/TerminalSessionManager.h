#pragma once

#include <contour/TerminalSession.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/helper.h>

#include <QtCore/QAbstractListModel>
#include <QtQml/QQmlEngine>

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
    void closeTab();
    void moveTabTo(int position);
    void moveTabToLeft(TerminalSession* session);
    void moveTabToRight(TerminalSession* session);

    void setSession(size_t index);

    void removeSession(TerminalSession&);

    Q_INVOKABLE [[nodiscard]] QVariant data(const QModelIndex& index,
                                            int role = Qt::DisplayRole) const override;
    Q_INVOKABLE [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(_sessions.size()); }

    void updateColorPreference(vtbackend::ColorPreference const& preference);
    display::TerminalDisplay* display = nullptr;
    TerminalSession* getSession() { return _sessions[0]; }

    void update() { updateStatusLine(); }

  private:
    contour::TerminalSession* activateSession(TerminalSession* session, bool isNewSession = false);
    std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string> cwd);

    [[nodiscard]] std::optional<std::size_t> getSessionIndexOf(TerminalSession* session) const noexcept
    {
        if (auto const i = std::ranges::find(_sessions, session); i != _sessions.end())
            return static_cast<std::size_t>(std::distance(_sessions.begin(), i));
        return std::nullopt;
    }

    [[nodiscard]] auto getCurrentSessionIndex() const noexcept
    {
        return getSessionIndexOf(_activeSession).value();
    }

    void updateStatusLine()
    {
        if (!_activeSession)
            return;
        _activeSession->terminal().setGuiTabInfoForStatusLine(vtbackend::TabsInfo {
            .tabs = std::ranges::transform_view(_sessions,
                                                [](auto* session) {
                                                    return vtbackend::TabsInfo::Tab {
                                                        .name = session->name(),
                                                        .color = vtbackend::RGBColor { 0, 0, 0 },
                                                    };
                                                })
                    | ranges::to<std::vector>(),
            .activeTabPosition = 1 + getSessionIndexOf(_activeSession).value_or(0),
        });
    }

    ContourGuiApp& _app;
    std::chrono::seconds _earlyExitThreshold;
    TerminalSession* _activeSession = nullptr;
    TerminalSession* _previousActiveSession = nullptr;
    std::vector<TerminalSession*> _sessions;
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSessionManager, "org.contour.TerminalSessionManager")
