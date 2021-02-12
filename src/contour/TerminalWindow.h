/**
 * This file is part of the "contour" project
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

#include <contour/Actions.h>
#include <contour/Config.h>
#include <contour/FileChangeWatcher.h>
#include <terminal/Metrics.h>
#include <terminal_view/TerminalView.h>

#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLWindow>
#include <QtGui/QVector4D>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QWidget>
#include <QtWidgets/QTabWidget>

#include <atomic>
#include <fstream>
#include <memory>

namespace contour {

class TerminalWidget;

// XXX Maybe just now a main window and maybe later just a TerminalWindow.
//
// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalWindow :
    public QMainWindow, // QTabWidget
    public terminal::view::TerminalView::Events
{
    Q_OBJECT

  public:
    TerminalWindow(config::Config _config, bool _liveConfig, std::string _profileName, std::string _programPath);
    ~TerminalWindow() override;

    //bool event(QEvent* _event) override;
    //bool focusNextPrevChild(bool) override;

  public Q_SLOTS:
    void onTerminalClosed(TerminalWidget* _terminalWidget);
    void setBackgroundBlur(bool _enable);

#if 0 // XXX if parent is QTabWidget
    void onTabChanged(int _index);
    TerminalWidget* newTab();
#endif

  private:
    TerminalWidget* createTerminalWidget();

  private:
    config::Config config_;
    const bool liveConfig_;
    std::string profileName_;
    std::string programPath_;
    TerminalWidget* terminalWidget_;
};

} // namespace contour
