/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <contour/Config.h>
#include <terminal/Process.h>

#include <QtCore/QThread>
#include <QtWidgets/QSystemTrayIcon>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace contour {

class TerminalSession;
class TerminalWindow;

class Controller : public QThread {
  public:
    Controller(std::string _programPath,
               std::chrono::seconds _earlyExitThreshold,
               contour::config::Config _config,
               bool _liveConfig,
               std::string _profileName);

    ~Controller();

    std::list<TerminalWindow*> const& terminalWindows() const noexcept { return terminalWindows_; }

    std::optional<terminal::Process::ExitStatus> exitStatus() const noexcept { return exitStatus_; }

    void onExit(TerminalSession& _session);

  public slots:
    void newWindow(contour::config::Config const& _config);
    void newWindow();
    void showNotification(QString const& _title, QString const& _content);

  private:
    static void onSigInt(int _signum);

  private:
    static Controller* self_;

    std::string programPath_;
    std::chrono::seconds earlyExitThreshold_;
    contour::config::Config config_;
    bool const liveConfig_;
    std::string profileName_;

    std::list<TerminalWindow*> terminalWindows_;

    // May contain the exit status of the last running window at exit.
    std::optional<terminal::Process::ExitStatus> exitStatus_;

    QSystemTrayIcon* systrayIcon_ = nullptr;
};

} // end namespace
