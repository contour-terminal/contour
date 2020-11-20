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

#include <qnamespace.h>
#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

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
#include <QScrollBar>

namespace contour {

using terminal::view::Renderer;
using actions::Action;

TerminalWindow::TerminalWindow(config::Config _config, string _profileName, string _programPath) :
    config_{ std::move(_config) },
    profileName_{ std::move(_profileName) },
    programPath_{ std::move(_programPath) }
{
    // QPalette p = QApplication::palette();
    // QColor backgroundColor(0x30, 0x30, 0x30, 0x80);
    // backgroundColor.setAlphaF(0.3);
    // p.setColor(QPalette::Window, backgroundColor);
    // setPalette(p);

    // connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground, false);

    setContentsMargins(0, 0, 0, 0);

    setTabBarAutoHide(true);
    connect(this, SIGNAL(currentChanged(int)), this, SLOT(onTabChanged(int)));

    newTab();

    //statusBar()->showMessage("blurb");
    // QVBoxLayout* verticalLayout = new QVBoxLayout(this);
    // verticalLayout->addWidget(terminalWidget_);
    // setLayout(verticalLayout);
    // verticalLayout->setContentsMargins(0, 0, 0, 0);
}

void TerminalWindow::newTab()
{
    auto const title = fmt::format("terminal {}", count() + 1);
    auto terminalWidget = new TerminalWidget(this, config_, profileName_, programPath_);

    if (count() && currentIndex() < count())
        insertTab(currentIndex() + 1, terminalWidget, title.c_str());
    else
        addTab(terminalWidget, title.c_str());

    connect(terminalWidget, SIGNAL(terminated(TerminalWidget*)), this, SLOT(onTerminalClosed(TerminalWidget*)));
    connect(terminalWidget, SIGNAL(setBackgroundBlur(bool)), this, SLOT(setBackgroundBlur(bool)));

    setCurrentWidget(terminalWidget);
}

TerminalWindow::~TerminalWindow()
{
}

void TerminalWindow::onTabChanged(int _index)
{
    if (_index < 0)
        return;

    auto tab = widget(_index);

    tab->setFocus();
}

void TerminalWindow::onTerminalClosed(TerminalWidget* _terminalWidget)
{
    int index = indexOf(_terminalWidget);
    std::cout << "TerminalWindow.onTerminalClosed(" << index << "): " << _terminalWidget->view()->terminal().screen().windowTitle() << '\n';
    if (index != -1)
        removeTab(index);

    if (count() == 0)
        close();
}

bool TerminalWindow::focusNextPrevChild(bool)
{
    return false;
}

void TerminalWindow::setBackgroundBlur([[maybe_unused]] bool _enable)
{
#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
    KWindowEffects::enableBlurBehind(winId(), _enable);
    KWindowEffects::enableBackgroundContrast(winId(), _enable);
#elif defined(_WIN32)
    // Awesome hack with the noteworty links:
    // * https://gist.github.com/ethanhs/0e157e4003812e99bf5bc7cb6f73459f (used as code template)
    // * https://github.com/riverar/sample-win32-acrylicblur/blob/master/MainWindow.xaml.cs
    // * https://stackoverflow.com/questions/44000217/mimicking-acrylic-in-a-win32-app
    // p.s.: if you find a more official way to do it, please PR me. :)

    if (HWND hwnd = (HWND)winId(); hwnd != nullptr)
    {
        const HINSTANCE hModule = LoadLibrary(TEXT("user32.dll"));
        if (hModule)
        {
            enum WindowCompositionAttribute : int {
                // ...
                WCA_ACCENT_POLICY = 19,
                // ...
            };
            enum AcceptState : int {
                ACCENT_DISABLED = 0,
                ACCENT_ENABLE_GRADIENT = 1,
                ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
                ACCENT_ENABLE_BLURBEHIND = 3,
                ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
                ACCENT_ENABLE_HOSTBACKDROP = 5,
            };
            struct ACCENTPOLICY
            {
                AcceptState nAccentState;
                int nFlags;
                int nColor;
                int nAnimationId;
            };
            struct WINCOMPATTRDATA
            {
                WindowCompositionAttribute nAttribute;
                void const* pData;
                ULONG ulDataSize;
            };
            typedef BOOL(WINAPI *pSetWindowCompositionAttribute)(HWND, WINCOMPATTRDATA const*);
            const pSetWindowCompositionAttribute SetWindowCompositionAttribute = (pSetWindowCompositionAttribute)GetProcAddress(hModule, "SetWindowCompositionAttribute");
            if (SetWindowCompositionAttribute)
            {
               auto const policy = _enable
                   ? ACCENTPOLICY{ ACCENT_ENABLE_BLURBEHIND, 0, 0, 0 }
                   : ACCENTPOLICY{ ACCENT_DISABLED, 0, 0, 0 };
               auto const data = WINCOMPATTRDATA{ WCA_ACCENT_POLICY, &policy, sizeof(ACCENTPOLICY) };
               BOOL rs = SetWindowCompositionAttribute(hwnd, &data);
               if (!rs)
                   qDebug() << "SetWindowCompositionAttribute" << rs;
            }
            FreeLibrary(hModule);
        }
    }
#else
   // Get me working on other platforms/compositors (such as OSX, Gnome, ...), please.
#endif
}

// void TerminalWindow::statsSummary()
// {
// #if defined(CONTOUR_VT_METRICS)
//     std::cout << "Some small summary in VT sequences usage metrics\n";
//     std::cout << "================================================\n\n";
//     for (auto const& [name, freq] : terminalMetrics_.ordered())
//         std::cout << fmt::format("{:>10}: {}\n", freq, name);
// #endif
// }

} // namespace contour
