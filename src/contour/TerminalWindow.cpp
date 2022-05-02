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
#include <contour/BackgroundBlur.h>
#include <contour/ContourGuiApp.h>
#include <contour/TerminalWindow.h>
#include <contour/helper.h>

#include <crispy/utils.h>
#if defined(CONTOUR_SCROLLBAR)
    #include <contour/ScrollableDisplay.h>
#endif

#include <contour/opengl/TerminalWidget.h>

#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>

#include <qnamespace.h>

#if defined(_MSC_VER)
    #include <terminal/pty/ConPty.h>
#else
    #include <terminal/pty/UnixPty.h>
#endif

#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QVBoxLayout>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
    #include <KWindowEffects>
#endif

#include <terminal/logging.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

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

struct SessionState
{
    std::string configPath;
    std::string profileName;
    std::string gridBuffer;
};

namespace
{
    SessionState loadSessionFile(FileSystem::path sessionFilePath)
    {
        auto sessionFile = std::ifstream(sessionFilePath.string());
        auto state = SessionState {};
        if (!sessionFile.is_open())
        {
            terminal::TerminalLog()("Failed to read session file: {}", sessionFilePath.string());
            return state;
        }
        sessionFile.unsetf(std::ios::skipws);
        std::getline(sessionFile, state.configPath);
        std::getline(sessionFile, state.profileName);
        std::copy(std::istream_iterator<char>(sessionFile),
                  std::istream_iterator<char>(),
                  std::back_inserter(state.gridBuffer));
        return state;
    }
} // namespace

TerminalWindow::TerminalWindow(std::chrono::seconds _earlyExitThreshold,
                               config::Config _config,
                               bool _liveConfig,
                               string _profileName,
                               string _programPath,
                               ContourGuiApp& _app):
    config_ { std::move(_config) },
    liveConfig_ { _liveConfig },
    profileName_ { std::move(_profileName) },
    programPath_ { std::move(_programPath) },
    app_ { _app }
{
    // connect(this, SIGNAL(screenChanged(QScreen*)), this, SLOT(onScreenChanged(QScreen*)));

    // QPalette p = QApplication::palette();
    // QColor backgroundColor(0x30, 0x30, 0x30, 0x80);
    // backgroundColor.setAlphaF(0.3);
    // p.setColor(QPalette::Window, backgroundColor);
    // setPalette(p);

    std::string _gridBuffer;
    if (profile().sessionResume && qApp->isSessionRestored())
    {
        auto const sessionFilePath =
            crispy::App::instance()->localStateDir()
            / (app_.parameters().get<std::string>("contour.terminal.session") + ".session");
        auto const [configPath, profile, gridBuffer] = loadSessionFile(sessionFilePath);
        if (!configPath.empty() && !profile.empty())
        {
            config_ = contour::config::loadConfigFromFile(configPath);
            profileName_ = profile;
            _gridBuffer = gridBuffer;
        }
        if (!FileSystem::remove(sessionFilePath))
            terminal::TerminalLog()("Failed to delete session file {}, file does not exist",
                                    sessionFilePath.string());
    }

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground, false);
    setWindowFlag(Qt::FramelessWindowHint, !profile().show_title_bar);

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
    if (config_.maxImageSize.width <= Width(0))
        config_.maxImageSize.width = defaultMaxImageSize.width;
    if (config_.maxImageSize.height <= Height(0))
        config_.maxImageSize.height = defaultMaxImageSize.height;
    // }}}

    auto shell = profile().shell;
#if defined(__APPLE__) || defined(_WIN32)
    {
        auto const path = FileSystem::path(programPath_).parent_path();

        if (shell.env.count("PATH"))
            shell.env["PATH"] += ":"s + path.string();
        else
            shell.env["PATH"] = path.string();
    }
#endif

    terminalSession_ = make_unique<TerminalSession>(
        make_unique<terminal::Process>(shell, terminal::createPty(profile().terminalSize, nullopt)),
        _earlyExitThreshold,
        config_,
        liveConfig_,
        profileName_,
        programPath_,
        app_,
        unique_ptr<TerminalDisplay> {},
        [this]() {
    // NB: This is invoked whenever the newly assigned display
    //     has finished initialization.
#if defined(CONTOUR_SCROLLBAR)
            scrollableDisplay_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
#else
            (void) this;
#endif
        },
        [this]() { app_.onExit(*terminalSession_); });
    if (qApp->isSessionRestored())
        terminalSession_->terminal().writeToScreen(_gridBuffer);

    terminalSession_->setDisplay(make_unique<opengl::TerminalWidget>(
        *terminalSession_,
        [this]() {
            centralWidget()->updateGeometry();
            update();
        },
        [this](bool _enable) { BlurBehind::setEnabled(windowHandle(), _enable); }));
    terminalWidget_ = static_cast<opengl::TerminalWidget*>(terminalSession_->display());

    connect(terminalWidget_, SIGNAL(terminated()), this, SLOT(onTerminalClosed()));
    connect(terminalWidget_,
            SIGNAL(terminalBufferChanged(terminal::ScreenType)),
            this,
            SLOT(terminalBufferChanged(terminal::ScreenType)));

#if defined(CONTOUR_SCROLLBAR)
    scrollableDisplay_ = new ScrollableDisplay(nullptr, *terminalSession_, terminalWidget_);
    setCentralWidget(scrollableDisplay_);
    connect(terminalWidget_, SIGNAL(terminalBufferUpdated()), scrollableDisplay_, SLOT(updateValues()));
#else
    setCentralWidget(terminalWidget_);
#endif

    terminalWidget_->setFocus();

    // statusBar()->showMessage("blurb");

    terminalSession_->start();
}

TerminalWindow::~TerminalWindow()
{
    DisplayLog()("~TerminalWindow");
}

void TerminalWindow::onTerminalClosed()
{
    DisplayLog()("terminal closed: {}", terminalSession_->terminal().windowTitle());
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

    if (terminalSession_->terminal().isPrimaryScreen())
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
