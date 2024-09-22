#! /bin/bash

set -ex

# Installs dependencies for running Contour GUI executable on CI

sudo apt install -y \
    \
    libfontconfig1 \
    libfreetype6 \
    libharfbuzz0b \
    libssh-4 \
    libyaml-cpp0.8 \
    \
    libqt6core5compat6 \
    libqt6gui6t64 \
    libqt6multimedia6 \
    libqt6multimediaquick6 \
    libqt6multimediawidgets6 \
    libqt6network6t64 \
    libqt6opengl6t64 \
    libqt6openglwidgets6t64 \
    libqt6qml6 \
    libqt6qmlcompiler6 \
    libqt6qmlcore6 \
    libqt6qmllocalstorage6 \
    libqt6qmlmodels6 \
    libqt6qmlworkerscript6 \
    libqt6qmlxmllistmodel6 \
    libqt6quick6 \
    libqt6quickcontrols2-6 \
    libqt6quickcontrols2impl6 \
    libqt6quickdialogs2-6 \
    libqt6quickdialogs2quickimpl6 \
    libqt6quickdialogs2utils6 \
    libqt6quicklayouts6 \
    libqt6quickparticles6 \
    libqt6quickshapes6 \
    libqt6quicktemplates2-6 \
    libqt6quicktest6 \
    libqt6quickwidgets6 \
    libqt6shadertools6 \
    \
    ncurses-bin ${@}
