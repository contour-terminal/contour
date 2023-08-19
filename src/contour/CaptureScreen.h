// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <string>

namespace contour
{

struct CaptureSettings
{
    bool logicalLines = false; // -l
    bool words = false;        // split output into one word per line
    double timeout = 1.0f;     // -t <timeout in seconds>
    std::string outputFile;    // -o <outputfile>
    int verbosityLevel = 0;    // -v, -q (XXX intentionally not parsed currently!)
    terminal::LineCount lineCount = terminal::LineCount { 0 }; // (use terminal default)
};

bool captureScreen(CaptureSettings const& _settings);

} // namespace contour
