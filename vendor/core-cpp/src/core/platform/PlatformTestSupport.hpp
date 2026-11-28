// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The platform questions the platform tests share. Test-only; never part of the module.

namespace core::platform::test
{

/// Whether this process runs under wine rather than on Windows or a POSIX system. Defined per
/// platform (`posix/PlatformTestSupport.cpp`, `windows/PlatformTestSupport.cpp`). A case uses it to
/// skip where wine differs from Windows in a way the case cannot tell from a defect, and says how.
/// A @c bool because the name asks it.
[[nodiscard]] bool runsUnderWine() noexcept;

} // namespace core::platform::test
