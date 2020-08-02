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
#include <contour/DebuggerService.h>

#include <QtCore/QThread>
#include <QtWidgets/QSystemTrayIcon>

#include <string>

#include <thread>
#include <memory>

namespace contour {

class TerminalWindow;

class Controller : public QThread {
  public:
    Controller(std::string _programPath,
               contour::config::Config _config,
               std::string _profileName);

    ~Controller();

    std::list<TerminalWindow*> const& terminalWindows() const noexcept { return terminalWindows_; }

  public slots:
    void newWindow();
    void showNotification(QString const& _title, QString const& _content);

  private:
    static void onSigInt(int _signum);
    void runDebugger();
    void debuggerMain();

  private:
    static Controller* self_;

    std::string programPath_;
    contour::config::Config config_;
    std::string profileName_;

    std::list<TerminalWindow*> terminalWindows_;
    std::unique_ptr<DebuggerService> debuggerService_;

    QSystemTrayIcon* systrayIcon_ = nullptr;
};

} // end namespace
