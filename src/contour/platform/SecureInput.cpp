// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/SecureInput.hpp>

// macOS defines makeSecureInput() in CocoaSecureInput.mm.
#ifndef __APPLE__

namespace contour::platform
{

std::unique_ptr<ISecureInput> makeSecureInput()
{
    return std::make_unique<NullSecureInput>();
}

} // namespace contour::platform

#endif
