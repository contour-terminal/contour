#pragma once

#include "Shader.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ERRORS_H

#include <GL/glew.h>
#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

class TextShaper {
public:
    TextShaper(std::string const& _fontFamily, unsigned int _fontSize, glm::mat4 const& _projectionMatrix);

    TextShaper(TextShaper&&) = delete;
    TextShaper(TextShaper const&) = delete;
    TextShaper& operator=(TextShaper&&) = delete;
    TextShaper& operator=(TextShaper const&) = delete;
    ~TextShaper();

    unsigned render(glm::ivec2 _pos, char32_t _char, float _r, float _g, float _b, float _opacity = 1.0f);
    unsigned render(unsigned _y, unsigned _x, char32_t _char, float _r, float _g, float _b, float _opacity = 1.0f);

    unsigned fontSize() const noexcept { return fontSize_; }
    unsigned lineHeight() const noexcept { return face_->size->metrics.height >> 6; }
    unsigned maxAdvance() const noexcept { return face_->size->metrics.max_advance >> 6; }
    unsigned baseline() const noexcept { return abs(face_->size->metrics.descender) >> 6; }

    void setProjection(glm::mat4 const& _projectionMatrix);

private:
    struct Glyph {
        GLuint texturedID;  // OpenGL texture ID
        glm::ivec2 size;    // glyph size
        glm::ivec2 bearing; // offset from baseline to left/top of glyph
        unsigned height;
        unsigned descender;
        unsigned advance;     // offset to advance to next glyph in line.
    };

    Glyph* getGlyph(char32_t _char);
	std::string freetypeErrorString(FT_Error _errorCode);
    static std::string const& vertexShaderCode();
    static std::string const& fragmentShaderCode();

private:
    FT_Library ft_{};
    FT_Face face_{};
    std::unordered_map<char32_t, Glyph> glyphCache_{};
    GLuint vao_{}; // vertex array object
    GLuint vbo_{}; // vertex buffer object
    unsigned fontSize_;
    Shader shader_;
};
