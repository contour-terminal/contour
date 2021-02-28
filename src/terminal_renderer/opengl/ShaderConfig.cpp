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
#include <terminal_renderer/opengl/ShaderConfig.h>

#include <crispy/logger.h>

#include <iostream>
#include <string>

#include "background_vert.h"
#include "background_frag.h"
#include "text_vert.h"
#include "text_frag.h"

namespace terminal::renderer::opengl {

namespace {
    auto const OpenGLRendererTag = crispy::debugtag::make("renderer.opengl", "Logs OpenGL render target details.");

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
            return {s(background_vert), s(background_frag), "builtin.background.vert", "builtin.background.frag"};
        case ShaderClass::Text:
            return {s(text_vert), s(text_frag), "builtin.text.vert", "builtin.text.frag"};
    }

    throw std::invalid_argument(fmt::format("ShaderClass<{}>", static_cast<unsigned>(_shaderClass)));
}

std::unique_ptr<QOpenGLShaderProgram> createShader(ShaderConfig const& _shaderConfig)
{
    auto shader = std::make_unique<QOpenGLShaderProgram>();

    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, _shaderConfig.vertexShader.c_str()))
    {
        debuglog(OpenGLRendererTag).write("Compiling vertex shader {} failed. {}", _shaderConfig.vertexShaderFileName,
                                                                  shader->log().toStdString());
        qDebug() << shader->log();
        return {};
    }

    if (!shader->addShaderFromSourceCode(QOpenGLShader::Fragment, _shaderConfig.fragmentShader.c_str()))
    {
        debuglog(OpenGLRendererTag).write("Compiling fragment shader {} failed. {}", _shaderConfig.fragmentShaderFileName,
                                                                    shader->log().toStdString());
        return {};
    }

    if (!shader->link())
    {
        debuglog(OpenGLRendererTag).write("Linking shaders {} & {} failed. {}",
                                          _shaderConfig.vertexShaderFileName,
                                          _shaderConfig.fragmentShaderFileName,
                                          shader->log().toStdString());
        return {};
    }

    if (auto const logString = shader->log().toStdString(); !logString.empty())
        debuglog(OpenGLRendererTag).write(logString);

    return shader;
}

} // end namespace
