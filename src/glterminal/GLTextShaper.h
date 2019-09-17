/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <glterminal/FontManager.h>
#include <glterminal/Shader.h>

#include <glm/glm.hpp>
#include <GL/glew.h>

#include <array>
#include <unordered_map>
#include <vector>

class GLTextShaper {
  public:
    GLTextShaper(Font& _regularFont, glm::mat4 const& _projection);
    ~GLTextShaper();

    void setProjection(glm::mat4 const& _projectionMatrix);

    void render(
        glm::ivec2 _pos,
        std::vector<char32_t> const& _chars,
        glm::vec4 const& _color,
        FontStyle _style);

  private:
    struct Glyph {
        GLuint textureID;
        glm::ivec2 size;      // glyph size
        glm::ivec2 bearing;   // offset from baseline to left/top of glyph
        unsigned height;
        unsigned descender;
        unsigned advance;     // offset to advance to next glyph in line.

        ~Glyph();
    };

    Glyph& getGlyphByIndex(unsigned long _index, FontStyle _style);

    static std::string const& fragmentShaderCode();
    static std::string const& vertexShaderCode();

  private:
    std::array<std::unordered_map<unsigned /*glyph index*/, Glyph>, 4> cache_;
    Font& regularFont_;
    std::vector<Font::GlyphPosition> glyphPositions_;
    GLuint vbo_;
    GLuint vao_;
    //glm::mat4 projectionMatrix_;
    Shader shader_;
    GLint colorLocation_;
};

