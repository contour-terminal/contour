#include "GLTextShaper.h"
#include "FontManager.h"

#include <GL/glew.h>

using namespace std;

GLTextShaper::Glyph::~Glyph()
{
    // TODO: release texture
}

GLTextShaper::GLTextShaper(Font& _regularFont, glm::mat4 const& _projection) :
    cache_{},
    regularFont_{ _regularFont },
    projectionMatrix_{ _projection },
    shader_{ vertexShaderCode(), fragmentShaderCode() }
{
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

    setProjection(_projection);
}

GLTextShaper::~GLTextShaper()
{
    // TODO: release vbo / vba
}

string const& GLTextShaper::vertexShaderCode()
{
	static string const code = R"(
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

string const& GLTextShaper::fragmentShaderCode()
{
	static string const code = R"(
		#version 330 core
		in vec2 TexCoords;
		out vec4 color;

		uniform sampler2D text;
		uniform vec4 textColor;

		void main()
		{
			vec4 sampled = vec4(1.0, 1.0, 1.0, texture(text, TexCoords).r);
			color = textColor * sampled;
		}
	)";
	return code;
}

void GLTextShaper::setProjection(glm::mat4 const& _projectionMatrix)
{
	shader_.use();
	shader_.setMat4("projection", _projectionMatrix);
}

void GLTextShaper::render(
    glm::ivec2 _pos,
    std::vector<char32_t> const& _chars,
    glm::vec4 const& _color,
    FontStyle _style)
{
    Font& font = regularFont_; // TODO: respect _style

    font.render(_chars, glyphPositions_);

    shader_.use();
    shader_.setVec4("textColor", _color);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    for (auto const& gpos : glyphPositions_)
    {
        if (gpos.codepoint == 0)
            continue;

        Glyph const& glyph = getGlyphByIndex(gpos.codepoint, _style);
        unsigned const x = _pos.x + gpos.x;
        unsigned const y = _pos.y + gpos.y;

        auto const xpos = static_cast<GLfloat>(x + glyph.bearing.x);
        auto const ypos = static_cast<GLfloat>(y + font.baseline() - glyph.descender);
        auto const w = static_cast<GLfloat>(glyph.size.x);
        auto const h = static_cast<GLfloat>(glyph.size.y);

        GLfloat const vertices[6][4] = {
            { xpos,     ypos + h,   0.0, 0.0 },
            { xpos,     ypos,       0.0, 1.0 },
            { xpos + w, ypos,       1.0, 1.0 },

            { xpos,     ypos + h,   0.0, 0.0 },
            { xpos + w, ypos,       1.0, 1.0 },
            { xpos + w, ypos + h,   1.0, 0.0 }
        };

        glBindTexture(GL_TEXTURE_2D, glyph.textureID);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glyphPositions_.clear();

    #if !defined(NDEBUG)
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    #endif
}

GLTextShaper::Glyph& GLTextShaper::getGlyphByIndex(unsigned long _index, FontStyle _style)
{
    auto& cache = cache_[static_cast<size_t>(_style)];
    if (auto i = cache.find(_index); i != cache.end())
        return i->second;

    Font& font = regularFont_; // TODO: respect _style
    font.loadGlyphByIndex(_index);

    // Generate texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RED,
        font->glyph->bitmap.width,
        font->glyph->bitmap.rows,
        0,
        GL_RED,
        GL_UNSIGNED_BYTE,
        font->glyph->bitmap.buffer
    );

    // Set texture options
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // store character for later use
    auto const descender = font->glyph->metrics.height / 64 - font->glyph->bitmap_top;
    Glyph& glyph = cache.emplace(make_pair(_index, Glyph{
        texture,
        glm::ivec2{(unsigned)font->glyph->bitmap.width, (unsigned)font->glyph->bitmap.rows},
        glm::ivec2{(unsigned)font->glyph->bitmap_left, (unsigned)font->glyph->bitmap_top},
        static_cast<unsigned>(font->height) / 64,
        static_cast<unsigned>(descender),
        static_cast<unsigned>(font->glyph->advance.x / 64)
    })).first->second;

    return glyph;
}
