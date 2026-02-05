// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/coretext_locator.h>
#include <text_shaper/directwrite_locator.h>
#include <text_shaper/font_locator_provider.h>
#include <text_shaper/fontconfig_locator.h>
#include <text_shaper/mock_font_locator.h>

#include <memory>

namespace text
{

using std::make_unique;

font_locator_provider& font_locator_provider::get()
{
    auto static instance = font_locator_provider {};
    return instance;
}

font_locator& font_locator_provider::native()
{
    if (!_native)
    {
#if defined(__APPLE__)
        _native = make_unique<coretext_locator>();
#elif defined(_WIN32)
        _native = make_unique<directwrite_locator>();
#else
        _native = make_unique<fontconfig_locator>();
#endif
    }
    return *_native;
}

font_locator& font_locator_provider::mock()
{
    if (!_mock)
        _mock = make_unique<mock_font_locator>();

    return *_mock;
}

} // namespace text
