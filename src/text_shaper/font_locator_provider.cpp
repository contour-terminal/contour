/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

#if defined(__APPLE__)
font_locator& font_locator_provider::coretext()
{
    if (!_coretext)
        _coretext = make_unique<coretext_locator>();

    return *_coretext;
}
#endif

#if defined(_WIN32)
font_locator& font_locator_provider::directwrite()
{
    if (!_directwrite)
        _directwrite = make_unique<directwrite_locator>();

    return *_directwrite;
}
#endif

font_locator& font_locator_provider::fontconfig()
{
    if (!_fontconfig)
        _fontconfig = make_unique<fontconfig_locator>();

    return *_fontconfig;
}

font_locator& font_locator_provider::mock()
{
    if (!_mock)
        _mock = make_unique<mock_font_locator>();

    return *_mock;
}

} // namespace text
