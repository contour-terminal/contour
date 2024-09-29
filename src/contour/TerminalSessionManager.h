#pragma once

#include <contour/TerminalSession.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/helper.h>

#include <QtCore/QAbstractListModel>
#include <QtQml/QQmlEngine>

#include <chrono>
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

    Q_INVOKABLE contour::TerminalSession* createSession();
    Q_INVOKABLE void addSession();

    Q_INVOKABLE void switchToTabLeft();
    Q_INVOKABLE void switchToTabRight();
    Q_INVOKABLE void switchToTab(int position);
    Q_INVOKABLE void closeTab();
    Q_INVOKABLE void setSession(size_t index);

    void removeSession(TerminalSession&);

    Q_INVOKABLE [[nodiscard]] QVariant data(const QModelIndex& index,
                                            int role = Qt::DisplayRole) const override;
    Q_INVOKABLE [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(_sessions.size()); }

    void updateColorPreference(vtbackend::ColorPreference const& preference);
    display::TerminalDisplay* display = nullptr;
    TerminalSession* getSession() { return _sessions[0]; }

  private:
    std::unique_ptr<vtpty::Pty> createPty();
    [[nodiscard]] auto getCurrentSessionIndex() const
    {
        return [](auto const& sessions, auto const& activeSession) {
            auto i =
                std::find_if(sessions.begin(), sessions.end(), [&](auto p) { return p == activeSession; });
            return i != sessions.end() ? i - sessions.begin() : -1;
        }(_sessions, _activeSession);
    }

    void updateStatusLine()
    {
        const auto currentSessionIndex = getCurrentSessionIndex();
        _activeSession->terminal().setGuiTabInfoForStatusLine([&]() {
            std::string tabInfo;
            for (size_t i = 0; i < _sessions.size(); ++i)
            {
                if (std::cmp_equal(i, currentSessionIndex))
                    tabInfo += "[";
                tabInfo += std::to_string(i + 1);
                if (std::cmp_equal(i, currentSessionIndex))
                    tabInfo += "]";
                tabInfo += " ";
            }
            return tabInfo;
        }());
    }

    bool isAllowedToChangeTabs()
    {
        // QML for some reason sends multiple signals requests in a row, so we need to ignore them.
        auto now = std::chrono::steady_clock::now();
        if (abs(now - _lastTabChange) < _timeBetweenTabSwitches)
        {
            managerLog()("Ignoring change request due to too frequent change requests.");
            return false;
        }
        return true;
    }

    ContourGuiApp& _app;
    std::chrono::seconds _earlyExitThreshold;
    TerminalSession* _activeSession = nullptr;
    std::vector<TerminalSession*> _sessions;
    std::chrono::time_point<std::chrono::steady_clock> _lastTabChange;
    std::chrono::milliseconds _timeBetweenTabSwitches { 300 };
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSessionManager, "org.contour.TerminalSessionManager")
