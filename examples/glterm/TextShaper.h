#pragma once

#include "Shader.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ERRORS_H

#if defined(_MSC_VER)
// XXX purely for IntelliSense
#include <freetype/freetype.h>
#endif

#include <GL/glew.h>
#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

class TextShaper {
public:
    TextShaper(std::string _fontPath, unsigned int _fontSize);

    TextShaper(TextShaper&&) = delete;
    TextShaper(TextShaper const&) = delete;
    TextShaper& operator=(TextShaper&&) = delete;
    TextShaper& operator=(TextShaper const&) = delete;
    ~TextShaper();

    unsigned render(glm::ivec2 _pos, char32_t _char, float _r, float _g, float _b);
    unsigned render(unsigned _y, unsigned _x, char32_t _char, float _r, float _g, float _b);

    unsigned short fontSize() const noexcept
    {
        return fontSize_;
    }

    unsigned lineHeight() const noexcept
    {
        return face_->size->metrics.height >> 6;
    }

    unsigned maxAdvance() const noexcept
    {
        return face_->size->metrics.max_advance >> 6;
    }

    Shader& shader() noexcept { return shader_; }

private:
    struct Glyph {
        GLuint texturedID;  // OpenGL texture ID
        glm::ivec2 size;    // glyph size
        glm::ivec2 bearing; // offset from baseline to left/top of glyph
        GLuint advance;     // offset to advance to next glyph in line.
    };

    Glyph* getGlyph(char32_t _char);

	std::string freetypeErrorString(FT_Error _errorCode);

    static std::string const& vertexShaderCode()
    {
        static std::string const code = R"(
            #version 330 core
            layout (location = 0) in vec4 vertex; // <vec2 pos, vec2 tex>
            out vec2 TexCoords;

            uniform mat4 projection;

            void main()
            {
                gl_Position = projection * vec4(vertex.xy, 0.1, 1.0);
                TexCoords = vertex.zw;
            }
        )";
        return code;
    }

    static std::string const& fragmentShaderCode()
    {
        static std::string const code = R"(
            #version 330 core
            in vec2 TexCoords;
            out vec4 color;

            uniform sampler2D text;
            uniform vec3 textColor;

            void main()
            {
                vec4 sampled = vec4(1.0, 1.0, 1.0, texture(text, TexCoords).r);
                color = vec4(textColor, 1.0) * sampled;
            }
        )";
        return code;
    }

private:
    FT_Library ft_{};
    FT_Face face_{};
    std::unordered_map<char32_t, Glyph> glyphCache_{};
    GLuint vao_{}; // vertex array object
    GLuint vbo_{}; // vertex buffer object
    unsigned short fontSize_;
    Shader shader_;
};

inline TextShaper::TextShaper(std::string _fontPath, unsigned int _fontSize) :
    shader_{ vertexShaderCode(), fragmentShaderCode() }
{
    if (FT_Init_FreeType(&ft_))
        throw std::runtime_error{ "Failed to initialize FreeType." };

    if (FT_New_Face(ft_, _fontPath.c_str(), 0, &face_)) // Consolas font on Windows
        throw std::runtime_error{ "Failed to load font." };

    FT_Error ec = FT_Select_Charmap(face_, FT_ENCODING_UNICODE);
    if (ec)
        throw std::runtime_error{ std::string{"Failed to set charmap. "} + freetypeErrorString(ec) };

    ec = FT_Set_Pixel_Sizes(face_, 0, _fontSize);
    if (ec)
        throw std::runtime_error{ std::string{"Failed to set font pixel size. "} + freetypeErrorString(ec) };

    // disable byte-alignment restriction
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Configure VAO/VBO for texture quads
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 6 * 4, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

inline TextShaper::~TextShaper()
{
    FT_Done_Face(face_);
    FT_Done_FreeType(ft_);
}

inline unsigned TextShaper::render(glm::ivec2 _pos, char32_t _char, float _r, float _g, float _b)
{
    return render(_pos[0], _pos[1], _char, _r, _g, _b);
}

inline unsigned TextShaper::render(unsigned _x, unsigned _y, char32_t _char, float _r, float _g, float _b)
{
    shader_.use();
    shader_.setVec3("textColor", glm::vec3{ _r, _g, _b });
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);

    //.
    GLfloat const scale = 1.0;
    Glyph const& ch = *getGlyph(_char);

    glColor3f(1.0, 1.0, 1.0);

    GLfloat xpos = _x + ch.bearing.x * scale;
    GLfloat ypos = _y - (ch.size.y - ch.bearing.y) * scale;

    GLfloat w = ch.size.x * scale;
    GLfloat h = ch.size.y * scale;

    // Update VBO for each character
    GLfloat vertices[6][4] = {
        { xpos,     ypos + h,   0.0, 0.0 },
        { xpos,     ypos,       0.0, 1.0 },
        { xpos + w, ypos,       1.0, 1.0 },

        { xpos,     ypos + h,   0.0, 0.0 },
        { xpos + w, ypos,       1.0, 1.0 },
        { xpos + w, ypos + h,   1.0, 0.0 }
    };

    // Render glyph texture over quad
    glBindTexture(GL_TEXTURE_2D, ch.texturedID);

    // Update content of VBO memory
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices); // Be sure to use glBufferSubData and not glBufferData

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Render quad
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Now advance cursors for next glyph (note that advance is number of 1/64 pixels)
    return static_cast<unsigned>((ch.advance / 64) * scale);
}

inline TextShaper::Glyph* TextShaper::getGlyph(char32_t _char)
{
    if (auto i = glyphCache_.find(_char); i != end(glyphCache_))
        return &i->second;

    //auto const glyphIndex = _char; // TODO: FT_Get_Char_Index(face_, _char);
    auto const glyphIndex = FT_Get_Char_Index(face_, _char);

    FT_Error ec = FT_Load_Glyph(face_, glyphIndex, FT_LOAD_RENDER);
    if (ec != FT_Err_Ok)
        throw std::runtime_error{ std::string{"Error loading glyph. "} + freetypeErrorString(ec) };

    // Generate texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RED,
        face_->glyph->bitmap.width,
        face_->glyph->bitmap.rows,
        0,
        GL_RED,
        GL_UNSIGNED_BYTE,
        face_->glyph->bitmap.buffer
    );

    // Set texture options
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // store character for later use
    Glyph& glyph = glyphCache_.emplace(std::make_pair(_char, Glyph{
        texture,
        glm::ivec2{(unsigned)face_->glyph->bitmap.width, (unsigned)face_->glyph->bitmap.rows},
        glm::ivec2{(unsigned)face_->glyph->bitmap_left, (unsigned)face_->glyph->bitmap_top},
        (unsigned)face_->glyph->advance.x
        })).first->second;

    glBindTexture(GL_TEXTURE_2D, 0);

    return &glyph;
}

inline std::string TextShaper::freetypeErrorString(FT_Error _errorCode)
{
    #undef __FTERRORS_H__
    #define FT_ERROR_START_LIST     switch (_errorCode) {
    #define FT_ERRORDEF( e, v, s )  case e: return s;
    #define FT_ERROR_END_LIST       }
    #include FT_ERRORS_H
    return "(Unknown error)";
}
