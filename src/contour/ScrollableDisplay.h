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
#pragma once

#include <QtWidgets/QWidget>
#include <QtWidgets/QScrollBar>

namespace contour {

class TerminalSession;

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

} // end namespace contour
