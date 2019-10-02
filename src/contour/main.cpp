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
#include "Contour.h"
#include "Flags.h"
#include "Config.h"

#include <terminal_view/GLLogger.h>
#include <terminal/InputGenerator.h>
#include <terminal/OutputGenerator.h>
#include <terminal/Terminal.h>
#include <terminal/Process.h>
#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <iostream>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

using namespace std;

int main(int argc, char const* argv[])
{
    try
    {
        auto config = Config{};
        if (auto exitStatus = loadConfigFromCLI(config, argc, argv); exitStatus.has_value())
            return *exitStatus;

        auto myterm = Contour{config};
        return myterm.main();
    }
    catch (exception const& e)
    {
        cerr << "Unhandled error caught. " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
