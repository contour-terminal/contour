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
#pragma once

#include <crispy/reference.h>
#include <crispy/text/Font.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ERRORS_H

#if defined(_MSC_VER)
// XXX purely for IntelliSense
#include <freetype/freetype.h>
#endif

#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>

namespace crispy::text {

/// API for managing multiple fonts.
class FontLoader {
  public:
    explicit FontLoader(std::ostream* logger = nullptr);
    FontLoader(FontLoader&&) = delete;
    FontLoader(FontLoader const&) = delete;
    FontLoader& operator=(FontLoader&&) = delete;
    FontLoader& operator=(FontLoader const&) = delete;
    ~FontLoader();

    FontList load(std::string const& _fontPattern, int _fontSize);

  private:
    Font* loadFromFilePath(std::string const& _filePath, int _fontSize);

  private:
    std::ostream* logger_;
    FT_Library ft_;
    std::unordered_map<std::string, Font> fonts_;
};

} // end namespace
