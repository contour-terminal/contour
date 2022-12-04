/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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
#include <contour/Actions.h>
#include <contour/BlurBehind.h>
#include <contour/ContourGuiApp.h>
#include <contour/TerminalWindow.h>
#include <contour/helper.h>

#if defined(CONTOUR_SCROLLBAR)
    #include <contour/ScrollableDisplay.h>
#endif

#include <contour/display/TerminalWidget.h>

#include <terminal/Metrics.h>

#include <vtpty/Pty.h>

#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QVBoxLayout>

#include <cstring>
#include <fstream>
#include <stdexcept>

#include <qnamespace.h>

using namespace std;
using namespace std::placeholders;

using terminal::Height;
using terminal::ImageSize;
using terminal::Width;

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

#include <QtWidgets/QStatusBar>

namespace contour
{

using actions::Action;

TerminalWindow::TerminalWindow(ContourGuiApp& _app): _app { _app }
{
    // connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));

    // QPalette p = QApplication::palette();
    // QColor backgroundColor(0x30, 0x30, 0x30, 0x80);
    // backgroundColor.setAlphaF(0.3);
    // p.setColor(QPalette::Window, backgroundColor);
    // setPalette(p);

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground, false);
    setWindowFlag(Qt::FramelessWindowHint, !profile().show_title_bar);

    setWindowIcon(QIcon(":/contour/logo.png"));

    // {{{ fill config's maxImageSize if not yet set.
    auto const defaultMaxImageSize = [&]() -> ImageSize {
        QScreen const* screen = QGuiApplication::primaryScreen();
        auto constexpr fallbackDefaultMaxImageSize = ImageSize { Width(800), Height(600) };
        if (!screen)
            return fallbackDefaultMaxImageSize;
        QSize const size = screen->size();
        if (size.isEmpty())
            return fallbackDefaultMaxImageSize;
        return ImageSize { Width::cast_from(size.width()), Height::cast_from(size.height()) };
    }();
    if (_app.config().maxImageSize.width <= Width(0))
        _app.config().maxImageSize.width = defaultMaxImageSize.width;
    if (_app.config().maxImageSize.height <= Height(0))
        _app.config().maxImageSize.height = defaultMaxImageSize.height;
    // }}}

    auto shell = profile().shell;
#if defined(__APPLE__) || defined(_WIN32)
    {
        auto const path = FileSystem::path(_app.programPath()).parent_path();

        if (shell.env.count("PATH"))
            shell.env["PATH"] += ":"s + path.string();
        else
            shell.env["PATH"] = path.string();
    }
#endif

    TerminalSession* session = _app.sessionsManager().createSession();

    terminalWidget_ = new display::TerminalWidget();

    connect(terminalWidget_, SIGNAL(terminated()), this, SLOT(onTerminalClosed()));
    connect(terminalWidget_,
            SIGNAL(terminalBufferChanged(terminal::ScreenType)),
            this,
            SLOT(terminalBufferChanged(terminal::ScreenType)));

#if defined(CONTOUR_SCROLLBAR)
    scrollableDisplay_ = new ScrollableDisplay(nullptr, terminalWidget_);
    setCentralWidget(scrollableDisplay_);
    connect(terminalWidget_, SIGNAL(terminalBufferUpdated()), scrollableDisplay_, SLOT(updateValues()));
#else
    setCentralWidget(terminalWidget_);
#endif

    connect(terminalWidget_, &display::TerminalWidget::displayInitialized, [this, session]() {
        session->attachDisplay(*terminalWidget_);
#if defined(CONTOUR_SCROLLBAR)
        scrollableDisplay_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        scrollableDisplay_->updatePosition();
#endif
        session->start();
    });

    terminalWidget_->setFocus();

    // statusBar()->showMessage("blurb");
}

config::TerminalProfile const& TerminalWindow::profile() const
{
    config::TerminalProfile const* theProfile = _app.config().profile(_app.profileName());
    Require(theProfile);
    return *theProfile;
}

TerminalWindow::~TerminalWindow()
{
    DisplayLog()("~TerminalWindow");
}

void TerminalWindow::onTerminalClosed()
{
    DisplayLog()("terminal closed: {}", terminalWidget_->session().terminal().windowTitle());
    close();
}

void TerminalWindow::setBlurBehind([[maybe_unused]] bool _enable)
{
    BlurBehind::setEnabled(windowHandle(), _enable);
}

void TerminalWindow::profileChanged()
{
#if defined(CONTOUR_SCROLLBAR)
    scrollableDisplay_->updatePosition();

    if (terminalWidget_->session().terminal().isPrimaryScreen())
        scrollableDisplay_->showScrollBar(profile().scrollbarPosition != config::ScrollBarPosition::Hidden);
    else
        scrollableDisplay_->showScrollBar(!profile().hideScrollbarInAltScreen);
#endif
}

void TerminalWindow::terminalBufferChanged(terminal::ScreenType _type)
{
#if defined(CONTOUR_SCROLLBAR)
    DisplayLog()("Screen buffer type has changed to {}.", _type);
    scrollableDisplay_->showScrollBar(_type == terminal::ScreenType::Primary
                                      || !profile().hideScrollbarInAltScreen);

    scrollableDisplay_->updatePosition();
    scrollableDisplay_->updateValues();
#else
    crispy::ignore_unused(_type);
#endif
}

void TerminalWindow::resizeEvent(QResizeEvent* _event)
{
    DisplayLog()("TerminalWindow.resizeEvent: size {}x{} ({}x{})",
                 width(),
                 height(),
                 _event->size().width(),
                 _event->size().height());

    QMainWindow::resizeEvent(_event);
    // centralWidget()->resize(_event->size());
    // updatePosition();
}

bool TerminalWindow::event(QEvent* _event)
{
    return QMainWindow::event(_event);
}

} // namespace contour
