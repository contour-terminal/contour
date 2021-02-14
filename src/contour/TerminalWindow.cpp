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
#include <contour/TerminalWindow.h>
#include <contour/Actions.h>
#include <contour/TerminalWidget.h>
#include <contour/BackgroundBlur.h>

#include <qnamespace.h>
#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

#include <crispy/logger.h>

#include <QtCore/QDebug>
#include <QtGui/QScreen>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QVBoxLayout>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
#include <KWindowEffects>
#endif

#include <cstring>
#include <fstream>
#include <stdexcept>

using namespace std;
using namespace std::placeholders;

#if defined(_MSC_VER)
#define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

#include <QStatusBar>

namespace contour {

using actions::Action;

TerminalWindow::TerminalWindow(config::Config _config, bool _liveConfig, string _profileName, string _programPath) :
    config_{ std::move(_config) },
    liveConfig_{ _liveConfig },
    profileName_{ std::move(_profileName) },
    programPath_{ std::move(_programPath) }
{
    // TODO: window frame's border should be the color of the window background?
    // QPalette p = QApplication::palette();
    // QColor backgroundColor(0x30, 0x30, 0x30, 0x80);
    // backgroundColor.setAlphaF(0.3);
    // p.setColor(QPalette::Window, backgroundColor);
    // setPalette(p);

    // connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground, false);

    // setTabBarAutoHide(true);
    // connect(this, SIGNAL(currentChanged(int)), this, SLOT(onTabChanged(int)));

    terminalWidget_ = createTerminalWidget();
    setCentralWidget(terminalWidget_);
    terminalWidget_->setFocus();

    //statusBar()->showMessage("blurb");
}

TerminalWindow::~TerminalWindow()
{
}

TerminalWidget* TerminalWindow::createTerminalWidget()
{
    auto terminalWidget = new TerminalWidget(config_, liveConfig_, profileName_, programPath_);

    connect(terminalWidget, SIGNAL(terminated(TerminalWidget*)), this, SLOT(onTerminalClosed(TerminalWidget*)));
    connect(terminalWidget, SIGNAL(setBackgroundBlur(bool)), this, SLOT(setBackgroundBlur(bool)));

    return terminalWidget;
}

#if 0
TerminalWidget* TerminalWindow::newTab()
{
    auto terminalWidget = createTerminalWidget();

    auto const title = fmt::format("terminal {}", count() + 1);
    if (count() && currentIndex() < count())
        insertTab(currentIndex() + 1, terminalWidget, title.c_str());
    else
        addTab(terminalWidget, title.c_str());

    setCurrentWidget(terminalWidget);
}

void TerminalWindow::onTabChanged(int _index)
{
    if (auto tab = widget(_index); tab != nullptr)
        tab->setFocus();
}

bool TerminalWindow::focusNextPrevChild(bool)
{
    return false;
}
#endif

void TerminalWindow::onTerminalClosed(TerminalWidget* _terminalWidget)
{
#if 0
    int index = indexOf(_terminalWidget);
    debuglog().write("index: {}; title {}", index, _terminalWidget->view()->terminal().screen().windowTitle());
    if (index != -1)
        removeTab(index);

    if (count() == 0)
        close();
#else
    (void) _terminalWidget;
    debuglog().write("title {}", _terminalWidget->view()->terminal().screen().windowTitle());
    close();
#endif
}

void TerminalWindow::setBackgroundBlur([[maybe_unused]] bool _enable)
{
    WindowBackgroundBlur::setEnabled(winId(), _enable);
}

// bool TerminalWindow::event(QEvent* _event)
// {
//     //qDebug() << "TerminalWindow.event:" << _event->type();
//     return QTabWidget::event(_event);
// }

} // namespace contour
