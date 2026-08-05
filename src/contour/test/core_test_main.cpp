// SPDX-License-Identifier: Apache-2.0
//
// Test runner for the Qt-free layers: the configuration model, the command vocabulary and the CLI.
//
// Deliberately not test_main.cpp: that one forces Qt's offscreen platform and constructs a
// QGuiApplication, because the GUI tests need one. These layers need neither, and a build
// configured with CONTOUR_FRONTEND_GUI=OFF has no Qt to give them -- which is why that
// configuration had no unit tests at all before this binary existed.

#include <crispy/SuppressWindowsDialogs.hpp>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    crispy::suppressWindowsDialogs();
    return Catch::Session().run(argc, argv);
}
