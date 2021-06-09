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
#include <contour/helper.h>

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

using actions::Action;

TerminalWindow::TerminalWindow(config::Config _config, bool _liveConfig, string _profileName, string _programPath) :
    config_{ std::move(_config) },
    liveConfig_{ _liveConfig },
    profileName_{ std::move(_profileName) },
    programPath_{ std::move(_programPath) }
{
    // connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));

    // QPalette p = QApplication::palette();
    // QColor backgroundColor(0x30, 0x30, 0x30, 0x80);
    // backgroundColor.setAlphaF(0.3);
    // p.setColor(QPalette::Window, backgroundColor);
    // setPalette(p);

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground, false);

#if defined(CONTOUR_SCROLLBAR)
    scrollBar_ = new QScrollBar(this);
    scrollBar_->setMinimum(0);
    scrollBar_->setMaximum(0);
    scrollBar_->setValue(0);
    scrollBar_->setCursor(Qt::ArrowCursor);
#endif

    terminalWidget_ = new TerminalWidget(config_, liveConfig_, profileName_, programPath_);

    connect(terminalWidget_, SIGNAL(terminated(TerminalWidget*)), this, SLOT(onTerminalClosed(TerminalWidget*)));
    connect(terminalWidget_, SIGNAL(setBackgroundBlur(bool)), this, SLOT(setBackgroundBlur(bool)));
    connect(terminalWidget_, SIGNAL(screenUpdated(TerminalWidget*)), this, SLOT(terminalScreenUpdated(TerminalWidget*)));
    connect(terminalWidget_, SIGNAL(profileChanged(TerminalWidget*)), this, SLOT(profileChanged(TerminalWidget*)));
    connect(terminalWidget_, SIGNAL(terminalBufferChanged(TerminalWidget*, terminal::ScreenType)), this, SLOT(terminalBufferChanged(TerminalWidget*, terminal::ScreenType)));

#if defined(CONTOUR_SCROLLBAR)
    connect(scrollBar_, &QScrollBar::valueChanged, this, QOverload<>::of(&TerminalWindow::onScrollBarValueChanged));

    layout_ = new QHBoxLayout();
    layout_->addWidget(terminalWidget_);

    if (config_.scrollbarPosition != config::ScrollBarPosition::Hidden)
        layout_->addWidget(scrollBar_);

    // TODO: this mainWidget could become its own contour terminal class that handles
    // therminal screen area as well as its scrollbar.
    QWidget* mainWidget = new QWidget();
    mainWidget->setLayout(layout_);
    layout_->setMargin(0);
    layout_->setSpacing(0);
    layout_->setContentsMargins(0, 0, 0, 0);
    setCentralWidget(mainWidget);
#else
    setCentralWidget(terminalWidget_);
#endif

    terminalWidget_->setFocus();

    //statusBar()->showMessage("blurb");
}

void TerminalWindow::updateScrollbarPosition()
{
#if defined(CONTOUR_SCROLLBAR)
    //terminalWidget_->setGeometry(calculateWidgetGeometry());
    debuglog(WindowTag).write("called with {}x{} in {}", width(), height(), terminalWidget_->screenType());

    if (terminalWidget_->screenType() != terminal::ScreenType::Alternate || !config_.hideScrollbarInAltScreen)
    {
        switch (config_.scrollbarPosition)
        {
            case config::ScrollBarPosition::Right:
                scrollBar_->show();
                layout_->removeWidget(scrollBar_);
                layout_->insertWidget(-1, scrollBar_);
                break;
            case config::ScrollBarPosition::Left:
                scrollBar_->show();
                layout_->removeWidget(scrollBar_);
                layout_->insertWidget(0, scrollBar_);
                break;
            case config::ScrollBarPosition::Hidden:
                layout_->removeWidget(scrollBar_);
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
        layout_->removeWidget(scrollBar_);
        scrollBar_->hide();
    }
#endif
}

void TerminalWindow::updateScrollbarValues()
{
#if defined(CONTOUR_SCROLLBAR)
    if (!scrollBar_->isVisible())
        return;

    scrollBar_->setMaximum(terminalWidget_->view()->terminal().screen().historyLineCount());
    if (auto const s = terminalWidget_->view()->terminal().viewport().absoluteScrollOffset(); s.has_value())
        scrollBar_->setValue(s.value());
    else
        scrollBar_->setValue(scrollBar_->maximum());
#endif
}

void TerminalWindow::onScrollBarValueChanged()
{
#if defined(CONTOUR_SCROLLBAR)
    if (scrollBar_->isSliderDown())
        terminalWidget_->onScrollBarValueChanged(scrollBar_->value());
#endif
}

QSize TerminalWindow::sizeHint() const
{
    auto result = QMainWindow::sizeHint();
#if defined(CONTOUR_SCROLLBAR)
    debuglog(WindowTag).write("{}x{}; widget: {}x{}, SBW: {}",
            result.width(),
            result.height(),
            terminalWidget_->sizeHint().width(),
            terminalWidget_->sizeHint().height(),
            scrollBar_->sizeHint().width()
    );
#endif
    return result;
}

void TerminalWindow::onTerminalClosed(TerminalWidget* _terminalWidget)
{
    debuglog(WindowTag).write("title {}", _terminalWidget->view()->terminal().screen().windowTitle());
    close();
}

void TerminalWindow::setBackgroundBlur([[maybe_unused]] bool _enable)
{
    WindowBackgroundBlur::setEnabled(winId(), _enable);
}

void TerminalWindow::profileChanged(TerminalWidget*)
{
#if defined(CONTOUR_SCROLLBAR)
    updateScrollbarPosition();

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
#endif
}

void TerminalWindow::terminalBufferChanged(TerminalWidget* /*_terminalWidget*/, terminal::ScreenType _type)
{
    debuglog(WindowTag).write("Screen buffer type has changed to {}.", _type);
#if defined(CONTOUR_SCROLLBAR)
    if (_type == terminal::ScreenType::Main)
        scrollBar_->show();
    else if (config_.hideScrollbarInAltScreen)
        scrollBar_->hide();
#endif

    updateScrollbarPosition();
    updateScrollbarValues();
}

void TerminalWindow::terminalScreenUpdated(TerminalWidget*)
{
    updateScrollbarValues();
}

void TerminalWindow::resizeEvent(QResizeEvent* _event)
{
    debuglog(WindowTag).write("new size {}x{}", width(), height());
    QMainWindow::resizeEvent(_event);
    updateScrollbarPosition();
}

bool TerminalWindow::event(QEvent* _event)
{
    //qDebug() << "TerminalWindow.event:" << _event->type();
    return QMainWindow::event(_event);
}

} // namespace contour
