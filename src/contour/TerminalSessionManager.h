#pragma once

#include <contour/TerminalSession.h>
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

    Q_INVOKABLE contour::TerminalSession* createSession();

    void removeSession(TerminalSession&);

    Q_INVOKABLE [[nodiscard]] QVariant data(const QModelIndex& index,
                                            int role = Qt::DisplayRole) const override;
    Q_INVOKABLE [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(_sessions.size()); }

    void updateColorPreference(vtbackend::ColorPreference const& preference);

  private:
    ContourGuiApp& _app;
    std::chrono::seconds _earlyExitThreshold;

    std::vector<TerminalSession*> _sessions;
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSessionManager, "org.contour.TerminalSessionManager")
