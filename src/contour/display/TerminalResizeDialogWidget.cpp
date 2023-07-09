#include "TerminalResizeDialogWidget.h"

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtCore/QTimer>

#include <iostream>
#include <qtimer.h>

TerminalResizeDialogWidget::TerminalResizeDialogWidget(QMainWindow* parent): parent_ {parent}
{
    this->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    label_ = new QLabel(this);
    auto font = QFont();
    font.setWeight(QFont::Bold);
    label_->setFont(font);

    layout_ = new QVBoxLayout(this);
    layout_->addWidget(label_);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

    showTimer_ = new QTimer(this);
    showTimer_->setSingleShot(true);
    showTimer_->setInterval(800);
    connect(showTimer_, &QTimer::timeout, [this]() { close(); });
}

void TerminalResizeDialogWidget::updateSize(const QSize& size)
{
    label_->setText(QString("%1 x %2").arg(size.width()).arg(size.height()));
    center();
    show();
    showTimer_->start();
}

void TerminalResizeDialogWidget::center()
{
    auto point = parent_->geometry().center();
    point.setX(point.x() - label_->width());
    point.setY(point.y() - label_->height());
    move(point);
}
