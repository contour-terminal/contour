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

#include <crispy/debuglog.h>

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

namespace {
    auto const WindowTag = crispy::debugtag::make("terminal.window", "Logs system window debug events.");
}

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

    createTerminalWidget();
    setCentralWidget(terminalWidget_);
    terminalWidget_->setFocus();

    //statusBar()->showMessage("blurb");
}

TerminalWindow::~TerminalWindow()
{
}

void TerminalWindow::createTerminalWidget()
{
    scrollBar_ = new QScrollBar(this);

    scrollBar_->setMinimum(0);
    scrollBar_->setMaximum(0);
    scrollBar_->setValue(0);
    scrollBar_->setCursor(Qt::ArrowCursor);

    terminalWidget_ = new TerminalWidget(config_, liveConfig_, profileName_, programPath_);
    recalculateGeometry();

    connect(scrollBar_, &QScrollBar::valueChanged, this, QOverload<>::of(&TerminalWindow::onScrollBarValueChanged));
    connect(terminalWidget_, SIGNAL(terminated(TerminalWidget*)), this, SLOT(onTerminalClosed(TerminalWidget*)));
    connect(terminalWidget_, SIGNAL(setBackgroundBlur(bool)), this, SLOT(setBackgroundBlur(bool)));
    connect(terminalWidget_, SIGNAL(screenUpdated(TerminalWidget*)), this, SLOT(terminalScreenUpdated(TerminalWidget*)));
    connect(terminalWidget_, SIGNAL(profileChanged(TerminalWidget*)), this, SLOT(profileChanged(TerminalWidget*)));
    connect(terminalWidget_, SIGNAL(terminalBufferChanged(TerminalWidget*, terminal::ScreenType)), this, SLOT(terminalBufferChanged(TerminalWidget*, terminal::ScreenType)));
}

void TerminalWindow::onScrollBarValueChanged()
{
    terminalWidget_->onScrollBarValueChanged(scrollBar_->value());
}

QRect TerminalWindow::calculateWidgetGeometry()
{
    QRect output(0, 0, width(), height());

    printf("calculateWidgetGeometry: width: %d\n", scrollBar_->width());
    switch (config_.scrollbarPosition)
    {
        case config::ScrollBarPosition::Left:
            printf("calculateWidgetGeometry: left\n");
            //output.setLeft(scrollBar_->width());
            output.setX(0);
            output.setWidth(width() - scrollBar_->sizeHint().width());
            break;
        case config::ScrollBarPosition::Right:
            printf("calculateWidgetGeometry: right\n");
            //output.setRight(output.right() - scrollBar_->sizeHint().width());
            output.setX(scrollBar_->sizeHint().width());
            output.setWidth(width() - scrollBar_->sizeHint().width());
            break;
        case config::ScrollBarPosition::Hidden:
            printf("calculateWidgetGeometry: hidden\n");
            break;
    }

    return output;
}

void TerminalWindow::recalculateGeometry()
{
    //terminalWidget_->setGeometry(calculateWidgetGeometry());
    debuglog(WindowTag).write("called with {}x{} in {}", width(), height(), terminalWidget_->screenType());

    if (terminalWidget_->screenType() != terminal::ScreenType::Alternate
       )//|| !config_.hideScrollbarInAltScreen)
    {
        scrollBar_->show();
        auto const ww = width();
        auto const wh = height();
        auto const scrollBarWidth = scrollBar_->sizeHint().width();

        switch (config_.scrollbarPosition)
        {
            case config::ScrollBarPosition::Right:
                scrollBar_->resize(scrollBarWidth, wh);
                scrollBar_->move(ww - scrollBarWidth, 0);
                terminalWidget_->move(0, 0);
                terminalWidget_->resize(ww - scrollBarWidth, wh);
                break;
            case config::ScrollBarPosition::Left:
                scrollBar_->resize(scrollBarWidth, height());
                scrollBar_->move(0, 0);
                terminalWidget_->move(scrollBar_->width(), 0);
                terminalWidget_->resize(width() - scrollBar_->width(), height());
                break;
            case config::ScrollBarPosition::Hidden:
                break;
        }
        debuglog(WindowTag).write("TW {}x{}+{}x{}, SB {}, {}x{}+{}x{}, value: {}/{}",
                terminalWidget_->pos().x(),
                terminalWidget_->pos().y(),
                terminalWidget_->width(),
                terminalWidget_->height(),
                scrollBar_->isVisible() ? "visible" : "invisible",
                scrollBar_->pos().x(),
                scrollBar_->pos().y(),
                scrollBar_->width(),
                scrollBar_->height(),
                scrollBar_->value(), scrollBar_->maximum());
    }
    else
    {
        debuglog(WindowTag).write("resize terminal widget over full contents");
        terminalWidget_->resize(width(), height());
        scrollBar_->hide();
    }
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
    debuglog(WindowTag).write("index: {}; title {}", index, _terminalWidget->view()->terminal().screen().windowTitle());
    if (index != -1)
        removeTab(index);

    if (count() == 0)
        close();
#else
    (void) _terminalWidget;
    debuglog(WindowTag).write("title {}", _terminalWidget->view()->terminal().screen().windowTitle());
    close();
#endif
}

void TerminalWindow::setBackgroundBlur([[maybe_unused]] bool _enable)
{
    WindowBackgroundBlur::setEnabled(winId(), _enable);
}

void TerminalWindow::profileChanged(TerminalWidget*)
{
    printf("profileChanged!\n");
    recalculateGeometry();
    //terminalWidget_->setGeometry(calculateWidgetGeometry());

    if (terminalWidget_->view()->terminal().screen().isPrimaryScreen())
    {
        switch (config_.scrollbarPosition)
        {
            case config::ScrollBarPosition::Left:
            case config::ScrollBarPosition::Right:
                scrollBar_->show();
                break;
            case config::ScrollBarPosition::Hidden:
                scrollBar_->hide();
                break;
        }
    }
    else
    {
        if (config_.hideScrollbarInAltScreen)
            scrollBar_->hide();
        else
            scrollBar_->show();
    }
}

void TerminalWindow::terminalBufferChanged(TerminalWidget* _terminalWidget, terminal::ScreenType _type)
{
    if (_type == terminal::ScreenType::Main)
        scrollBar_->show();
    else //if (config_.hideScrollbarInAltScreen)
        scrollBar_->hide();

    recalculateGeometry();
    viewportChanged(_terminalWidget);
}

void TerminalWindow::viewportChanged(TerminalWidget*)
{
    if (!scrollBar_->isVisible())
        return;

    scrollBar_->setMaximum(terminalWidget_->view()->terminal().screen().historyLineCount());
    if (auto const s = terminalWidget_->view()->terminal().viewport().absoluteScrollOffset(); s.has_value())
        scrollBar_->setValue(s.value());
}

void TerminalWindow::terminalScreenUpdated(TerminalWidget*)
{
    if (terminalWidget_->view()->terminal().screen().isPrimaryScreen())
    {
        scrollBar_->setMaximum(terminalWidget_->view()->terminal().screen().historyLineCount());
        if (auto const s = terminalWidget_->view()->terminal().viewport().absoluteScrollOffset(); s.has_value())
            scrollBar_->setValue(s.value());
        else
            scrollBar_->setValue(scrollBar_->maximum());

        debuglog(WindowTag).write("TW {}x{}+{}x{}, SB {}, {}x{}+{}x{}, value: {}/{}",
                terminalWidget_->pos().x(),
                terminalWidget_->pos().y(),
                terminalWidget_->width(),
                terminalWidget_->height(),
                scrollBar_->isVisible() ? "visible" : "invisible",
                scrollBar_->pos().x(),
                scrollBar_->pos().y(),
                scrollBar_->width(),
                scrollBar_->height(),
                scrollBar_->value(), scrollBar_->maximum());
    }
}

void TerminalWindow::resizeEvent(QResizeEvent* _event)
{
    debuglog(WindowTag).write("new size {}x{}", width(), height());
    QMainWindow::resizeEvent(_event);
    recalculateGeometry();
}

bool TerminalWindow::event(QEvent* _event)
{
    //qDebug() << "TerminalWindow.event:" << _event->type();
    return QMainWindow::event(_event);
}

} // namespace contour
