// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/FontLocatorProvider.hpp>

#include <text_shaper/CoreTextLocator.hpp>
#include <text_shaper/DirectWriteLocator.hpp>
#include <text_shaper/FontconfigLocator.hpp>
#include <text_shaper/MockFontLocator.hpp>

#include <memory>

namespace text
{

using std::make_unique;

FontLocatorProvider& FontLocatorProvider::get()
{
    auto static instance = FontLocatorProvider {};
    return instance;
}

FontLocator& FontLocatorProvider::native()
{
    if (!_native)
    {
#ifdef __APPLE__
        _native = make_unique<CoreTextLocator>();
#elifdef _WIN32
        _native = make_unique<DirectWriteLocator>();
#else
        _native = make_unique<FontconfigLocator>();
#endif
    }
    return *_native;
}

FontLocator& FontLocatorProvider::mock()
{
    if (!_mock)
        _mock = make_unique<MockFontLocator>();

    return *_mock;
}

} // namespace text
