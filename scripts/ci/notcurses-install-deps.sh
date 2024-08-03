#! /bin/bash
set -ex


RELEASE=`grep VERSION_ID /etc/os-release | cut -d= -f2 | tr -d '"'`

if [ "$RELEASE" = "20.04" ]; then
    packages="
            xvfb \
            \
            ffmpeg \
            libavcodec58 \
            libavdevice58 \
            libavformat58 \
            libavutil56 \
            libdeflate0 \
            libncurses6 \
            libqrcodegen1 \
            libswscale5 \
            libunistring2 \
            \
            libfontconfig1 \
            libfreetype6 \
            libharfbuzz0b \
            \
            libqt5core5a \
            libqt5gui5 \
            libqt5gui5 \
            libqt5multimedia5 \
            libqt5multimedia5-plugins \
            libqt5network5 \
            libqt5opengl5-dev \
            libqt5x11extras5 \
            libqt5x11extras5-dev \
            qml-module-qt-labs-platform \
            qml-module-qtmultimedia \
            qml-module-qtquick-controls \
            qml-module-qtquick-controls2 \
            qtbase5-dev \
            qtdeclarative5-dev \
            qtmultimedia5-dev \
            qtquickcontrols2-5-dev \
            \
            libutempter0 \
            libyaml-cpp0.6 \
            libc6
    "
elif [ "$RELEASE" = "22.04" ]; then
    packages="
            libqt6core5compat6 \
            libqt6gui6 \
            libqt6multimedia6 \
            libqt6multimediaquick6 \
            libqt6multimediawidgets6 \
            libqt6opengl6 \
            libqt6openglwidgets6 \
            libqt6qml6 \
            libqt6quick6 \
            \
            qml6-module-qt-labs-platform \
            qml6-module-qt5compat-graphicaleffects \
            qml6-module-qtmultimedia \
            qml6-module-qtqml-workerscript \
            qml6-module-qtquick-controls \
            qml6-module-qtquick-layouts \
            qml6-module-qtquick-templates \
            qml6-module-qtquick-window \
            qt6-qpa-plugins \
            \
            xvfb \
            \
            ffmpeg \
            libavcodec58 \
            libavdevice58 \
            libavformat58 \
            libavutil56 \
            libdeflate0 \
            libncurses6 \
            libqrcodegen1 \
            libswscale5 \
            libunistring2 \
            \
            libfontconfig1 \
            libfreetype6 \
            libharfbuzz0b \
            \
            libqt5core5a \
            libqt5gui5 \
            libqt5gui5 \
            libqt5multimedia5 \
            libqt5multimedia5-plugins \
            libqt5network5 \
            libqt5opengl5-dev \
            libqt5x11extras5 \
            libqt5x11extras5-dev \
            qml-module-qt-labs-platform \
            qml-module-qtmultimedia \
            qml-module-qtquick-controls \
            qml-module-qtquick-controls2 \
            qtbase5-dev \
            qtdeclarative5-dev \
            qtmultimedia5-dev \
            qtquickcontrols2-5-dev \
            \
            libutempter0 \
            libyaml-cpp0.7

    "
else # 24.04
packages="
          libqt6core5compat6 \
          libqt6gui6 \
          libqt6multimedia6 \
          libqt6multimediaquick6 \
          libqt6multimediawidgets6 \
          libqt6opengl6 \
          libqt6openglwidgets6 \
          libqt6qml6 \
          libqt6quick6 \
          \
          qml6-module-qt-labs-platform \
          qml6-module-qt5compat-graphicaleffects \
          qml6-module-qtmultimedia \
          qml6-module-qtqml-workerscript \
          qml6-module-qtquick-controls \
          qml6-module-qtquick-layouts \
          qml6-module-qtquick-templates \
          qml6-module-qtquick-window \
          qt6-qpa-plugins \
          \
          xvfb \
          \
          ffmpeg \
          libavcodec-dev \
          libavdevice-dev \
          libavformat-dev \
          libavutil-dev \
          libdeflate-dev \
          libncurses-dev \
          libqrcodegen-dev \
          libswscale-dev \
          libunistring-dev \
          \
          libfontconfig1 \
          libfreetype6 \
          libharfbuzz0b \
          \
          libqt5core5a \
          libqt5gui5 \
          libqt5gui5 \
          libqt5multimedia5 \
          libqt5multimedia5-plugins \
          libqt5network5 \
          libqt5opengl5-dev \
          libqt5x11extras5 \
          libqt5x11extras5-dev \
          qml-module-qt-labs-platform \
          qml-module-qtmultimedia \
          qml-module-qtquick-controls \
          qml-module-qtquick-controls2 \
          qtbase5-dev \
          qtdeclarative5-dev \
          qtmultimedia5-dev \
          qtquickcontrols2-5-dev \
          \
          libutempter0 \
          libyaml-cpp0.8 \
          build-essential \
          cmake \
          doctest-dev
"
fi

sudo apt install -y $packages
