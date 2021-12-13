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

#include <contour/ScrollableDisplay.h>
#include <contour/TerminalSession.h>
#include <contour/helper.h>

#include <terminal/Terminal.h>
#include <terminal/Viewport.h>

namespace contour {

ScrollableDisplay::ScrollableDisplay(QWidget* _parent, TerminalSession& _session, QWidget* _main):
    QWidget(_parent),
    session_{ _session },
    mainWidget_{ _main },
    scrollBar_{ nullptr }
{
    mainWidget_->setParent(this);

    scrollBar_ = new QScrollBar(this);
    scrollBar_->setMinimum(0);
    scrollBar_->setMaximum(0);
    scrollBar_->setValue(0);
    scrollBar_->setCursor(Qt::ArrowCursor);

    connect(scrollBar_, &QScrollBar::valueChanged,
            this, QOverload<>::of(&ScrollableDisplay::onValueChanged));

    QSize ms = mainWidget_->sizeHint();
    QSize ss = scrollBar_->sizeHint();
    ms.setWidth(width() - ss.width());
    ss.setHeight(height());
    scrollBar_->resize(ss);
    mainWidget_->resize(ms);

    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    updateGeometry();
}

QSize ScrollableDisplay::sizeHint() const
{
    QSize s = mainWidget_->sizeHint();
    s.setWidth(s.width() + scrollBar_->sizeHint().width());
    return s;
}

void ScrollableDisplay::resizeEvent(QResizeEvent* _event)
{
    QWidget::resizeEvent(_event);

    auto const sbWidth = scrollBar_->width();
    auto const mainWidth = width() - sbWidth;
    mainWidget_->resize(mainWidth, height());
    scrollBar_->resize(sbWidth, height());
    // mainWidget_->move(0, 0);
    // scrollBar_->move(mainWidth, 0);
    updatePosition();
    updateGeometry();
}

void ScrollableDisplay::showScrollBar(bool _show)
{
    if (_show)
        scrollBar_->show();
    else
        scrollBar_->hide();
}

void ScrollableDisplay::updateValues()
{
    if (!scrollBar_->isVisible())
        return;

    scrollBar_->setMaximum(session_.terminal().screen().historyLineCount().as<int>());
    auto const s = session_.terminal().viewport().scrollOffset();
    scrollBar_->setValue(scrollBar_->maximum() - s.value);
}

void ScrollableDisplay::updatePosition()
{
    //terminalWidget_->setGeometry(calculateWidgetGeometry());
    LOGSTORE(DisplayLog)("called with {}x{} in {}", width(), height(),
                         session_.currentScreenType());

    auto const resizeMainAndScrollArea = [&]() {
        QSize ms = mainWidget_->sizeHint();
        QSize ss = scrollBar_->sizeHint();
        ms.setWidth(width() - ss.width());
        ms.setHeight(height());
        ss.setHeight(height());
        scrollBar_->resize(ss);
        mainWidget_->resize(ms);
    };

    if (session_.currentScreenType() != terminal::ScreenType::Alternate
        || !session_.profile().hideScrollbarInAltScreen)
    {
        auto const sbWidth = scrollBar_->width();
        auto const mainWidth = width() - sbWidth;
        LOGSTORE(DisplayLog)("Scrollbar Pos: {}", session_.profile().scrollbarPosition);
        switch (session_.profile().scrollbarPosition)
        {
            case config::ScrollBarPosition::Right:
                resizeMainAndScrollArea();
                scrollBar_->show();
                mainWidget_->move(0, 0);
                scrollBar_->move(mainWidth, 0);
                break;
            case config::ScrollBarPosition::Left:
                resizeMainAndScrollArea();
                scrollBar_->show();
                mainWidget_->move(sbWidth, 0);
                scrollBar_->move(0, 0);
                break;
            case config::ScrollBarPosition::Hidden:
            {
                scrollBar_->hide();
                auto const cr = contentsRect();
                mainWidget_->resize(cr.right(), cr.bottom());
                mainWidget_->move(0, 0);
                break;
            }
        }
        LOGSTORE(DisplayLog)(
            "TW {}x{}+{}x{}, SB {}, {}x{}+{}x{}, value: {}/{}",
            mainWidget_->pos().x(),
            mainWidget_->pos().y(),
            mainWidget_->width(),
            mainWidget_->height(),
            scrollBar_->isVisible() ? "visible" : "invisible",
            scrollBar_->pos().x(),
            scrollBar_->pos().y(),
            scrollBar_->width(),
            scrollBar_->height(),
            scrollBar_->value(), scrollBar_->maximum()
        );
    }
    else
    {
        LOGSTORE(DisplayLog)("Resize terminal widget over full contents.");
        scrollBar_->hide();
    }
}

void ScrollableDisplay::onValueChanged()
{
    session_.terminal().viewport().scrollTo(
        terminal::ScrollOffset::cast_from(scrollBar_->maximum() - scrollBar_->value())
    );
    session_.scheduleRedraw();
}

} // end namespace contour
