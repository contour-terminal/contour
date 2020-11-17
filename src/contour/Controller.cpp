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
#include <contour/Controller.h>
#include <contour/TerminalWindow.h>

#include <QtCore/QProcess>
#include <QtGui/QGuiApplication>

using namespace std;

namespace contour {

Controller* Controller::self_ = nullptr;

Controller::Controller(std::string _programPath,
                       config::Config _config,
                       std::string _profileName) :
    programPath_{ move(_programPath) },
    config_{ move(_config) },
    profileName_{ move(_profileName) }
{
    // systrayIcon_ = new QSystemTrayIcon(nullptr);
    // systrayIcon_->show();
    // TODO: systrayIcon_: add icon
    // TODO: systrayIcon_: add context menu?

    connect(this, &Controller::started, this, [this]() { newWindow(); });

    self_ = this;
}

Controller::~Controller()
{
}

void Controller::newWindow()
{
    auto mainWindow = new TerminalWindow{
        config_,
        profileName_,
        programPath_
    };
    mainWindow->show();

    terminalWindows_.push_back(mainWindow);
    // TODO: Remove window from list when destroyed.

    // QObject::connect(mainWindow, &TerminalWindow::showNotification,
    //                  this, &Controller::showNotification);

}

void Controller::showNotification(QString const& _title, QString const& _content)
{
    // systrayIcon_->showMessage(
    //     _title,
    //     _content,
    //     QSystemTrayIcon::MessageIcon::Information,
    //     10 * 1000
    // );

#if defined(__linux__)
    // XXX requires notify-send to be installed.
    QStringList args;
    args.append("--urgency=low");
    args.append("--expire-time=10000");
    args.append("--category=terminal");
    args.append(_title);
    args.append(_content);
    QProcess::execute(QString::fromLatin1("notify-send"), args);
#elif defined(__APPLE__)
    // TODO: use Growl?
#elif defined(_WIN32)
    // TODO: use Toast
#endif
}

} // end namespace
