// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NativeKeyTrace.hpp>

#include <utility>

// macOS defines makeNativeKeyTrace() in CocoaNativeKeyTrace.mm.
#ifndef __APPLE__

namespace contour::platform
{

std::unique_ptr<INativeKeyTrace> makeNativeKeyTrace(NativeKeyTraceSink sink)
{
    auto const report = std::move(sink);
    report("The native key trace is available on macOS only.");
    return std::make_unique<INativeKeyTrace>();
}

} // namespace contour::platform

#endif
