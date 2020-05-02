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
#include <terminal_view/ShaderConfig.h>

#include <string>

#include "background_vert.h"
#include "background_frag.h"
#include "cursor_vert.h"
#include "cursor_frag.h"
#include "text_vert.h"
#include "text_frag.h"

namespace terminal::view {

namespace {
    template <size_t N>
    inline std::string s(std::array<uint8_t, N> const& data)
    {
        return std::string(reinterpret_cast<char const*>(data.data()), data.size());
    }
}

ShaderConfig defaultShaderConfig(ShaderClass _shaderClass)
{
    using namespace default_shaders;

    switch (_shaderClass)
    {
        case ShaderClass::Background:
            return {s(background_vert), s(background_frag)};
        case ShaderClass::Text:
            return {s(text_vert), s(text_frag)};
        case ShaderClass::Cursor:
            return {s(cursor_vert), s(cursor_frag)};
    }

    throw std::invalid_argument(fmt::format("ShaderClass<{}>", static_cast<unsigned>(_shaderClass)));
}

std::unique_ptr<QOpenGLShaderProgram> createShader(ShaderConfig const& _shaderConfig)
{
    auto shader = std::make_unique<QOpenGLShaderProgram>();
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, _shaderConfig.vertexShader.c_str()))
    {
        qDebug() << shader->log();
        return {};
    }
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Fragment, _shaderConfig.fragmentShader.c_str()))
    {
        qDebug() << shader->log();
        return {};
    }
    if (!shader->link())
    {
        qDebug() << shader->log();
        return {};
    }

    return shader;
}

} // end namespace
