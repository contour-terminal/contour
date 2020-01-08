/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <contour/Config.h>
#include <contour/TerminalWindow.h>
#include <QGuiApplication>
#include <iostream>

using namespace std;

int main(int argc, char* argv[])
{
    try
    {
        QGuiApplication::setApplicationName("contour");
        QGuiApplication::setOrganizationName("contour");
        QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
        QGuiApplication app(argc, argv);

        // TODO: CLI
        // --help
        // -c,--config=PATH
        // -p,--profile=NAME
        // ??? -s,--shell=SHELL
        // ??? [ -- args for shell, if provided]

        auto mainWindow = contour::TerminalWindow{contour::loadConfig(), argv[0]};
        mainWindow.show();

        return app.exec();
    }
    catch (exception const& e)
    {
        cerr << "Unhandled error caught. " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
