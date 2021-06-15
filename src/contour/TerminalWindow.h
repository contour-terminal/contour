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
#include <contour/TerminalDisplay.h>
#include <contour/TerminalSession.h>
#include <contour/opengl/TerminalWidget.h>

#include <terminal/Metrics.h>

#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLWindow>
#include <QtGui/QVector4D>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QWidget>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QHBoxLayout>

#if defined(CONTOUR_SCROLLBAR)
#include <QtWidgets/QScrollBar>
#endif

#include <atomic>
#include <fstream>
#include <memory>

namespace contour {

#if defined(CONTOUR_SCROLLBAR)
class ScrollableDisplay: public QWidget
{
    Q_OBJECT

public:
    ScrollableDisplay(QWidget* _parent, TerminalSession& _session, QWidget* _main);

    QSize sizeHint() const override;
    void resizeEvent(QResizeEvent* _event) override;

    void showScrollBar(bool _show);
    void updatePosition();

public Q_SLOTS:
    void updateValues();
    void onValueChanged();

private:
    TerminalSession& session_;
    QWidget* mainWidget_;
    QScrollBar* scrollBar_;
};
#endif

// XXX Maybe just now a main window and maybe later just a TerminalWindow.
//
// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalWindow :
    public QMainWindow
{
    Q_OBJECT

  public:
    TerminalWindow(config::Config _config, bool _liveConfig, std::string _profileName, std::string _programPath);

    bool event(QEvent* _event) override;
    void resizeEvent(QResizeEvent* _event) override;

    //QSize sizeHint() const override;

  public Q_SLOTS:
    void terminalBufferChanged(terminal::ScreenType);
    void profileChanged();
    void onTerminalClosed();
    void setBackgroundBlur(bool _enable);

  private:
    // data members
    //
    config::Config config_;
    const bool liveConfig_;
    std::string profileName_;
    std::string programPath_;

#if defined(CONTOUR_SCROLLBAR)
    ScrollableDisplay* scrollableDisplay_ = nullptr;
#endif

    std::unique_ptr<TerminalSession> terminalSession_;
    opengl::TerminalWidget* terminalWidget_ = nullptr;
};

} // namespace contour
