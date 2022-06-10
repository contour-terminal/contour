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
#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/display/TerminalWidget.h>

#include <terminal/Metrics.h>

#include <crispy/assert.h>

#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLExtraFunctions>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGL/QOpenGLWindow>
#else
    #include <QtGui/QOpenGLWindow>
#endif
#include <QtGui/QVector4D>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>

#include <atomic>
#include <fstream>
#include <memory>

namespace contour
{

class ContourGuiApp;
class ScrollableDisplay;

// XXX Maybe just now a main window and maybe later just a TerminalWindow.
//
// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalWindow: public QMainWindow
{
    Q_OBJECT

  public:
    TerminalWindow(ContourGuiApp& _app);
    ~TerminalWindow() override;

    bool event(QEvent* _event) override;
    void resizeEvent(QResizeEvent* _event) override;

    // QSize sizeHint() const override;

    [[nodiscard]] config::TerminalProfile const& profile() const;

  public Q_SLOTS:
    void terminalBufferChanged(terminal::ScreenType);
    void profileChanged();
    void onTerminalClosed();
    void setBlurBehind(bool _enable);

  private:
    ContourGuiApp& _app;

#if defined(CONTOUR_SCROLLBAR)
    ScrollableDisplay* scrollableDisplay_ = nullptr;
#endif

    display::TerminalWidget* terminalWidget_ = nullptr;
};

} // namespace contour
