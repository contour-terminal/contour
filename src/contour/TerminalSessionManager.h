#pragma once

#include <contour/TerminalSession.h>

#include <vector>

namespace contour
{

/**
 * Manages terminal sessions.
 */
class TerminalSessionManager: public QObject
{
    Q_OBJECT

  public:
    TerminalSessionManager(ContourGuiApp& app);

    TerminalSession* createSession();

    void removeSession(TerminalSession&);

  private:
    ContourGuiApp& _app;
    std::chrono::seconds _earlyExitThreshold;

    std::vector<TerminalSession*> _sessions;
};

} // namespace contour
