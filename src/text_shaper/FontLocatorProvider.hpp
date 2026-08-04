// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/FontLocator.hpp>

#include <memory>
#include <mutex>

namespace text
{

/// Provides access to platform-native and mock font locators.
///
/// The instance is process-wide, and a locator is reached from every renderer's shaper — which, with one
/// renderer per split pane and one render thread per window, means from several threads at once. The lazy
/// construction below is therefore serialized; the locator implementations themselves are expected to be
/// safe to *use* concurrently, which the platform back-ends (fontconfig, DirectWrite, CoreText) are.
///
/// Prefer taking a FontLocator& as a constructor parameter over calling get() from inside a class: the
/// singleton is a composition-root convenience, not an ambient dependency, and a class that reaches for it
/// directly cannot be tested against MockFontLocator.
class FontLocatorProvider
{
  public:
    static FontLocatorProvider& get();

    /// Returns the native font locator, initializing it lazily if necessary.
    FontLocator& native();

    FontLocator& mock();

  private:
    std::once_flag _nativeOnce;
    std::once_flag _mockOnce;
    std::unique_ptr<FontLocator> _native {};
    std::unique_ptr<FontLocator> _mock {};
};

} // namespace text
