// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/FontLocator.hpp>

#include <memory>

namespace text
{

/// Provides access to platform-native and mock font locators.
class FontLocatorProvider
{
  public:
    static FontLocatorProvider& get();

    /// Returns the native font locator, initializing it lazily if necessary.
    FontLocator& native();

    FontLocator& mock();

  private:
    std::unique_ptr<FontLocator> _native {};
    std::unique_ptr<FontLocator> _mock {};
};

} // namespace text
