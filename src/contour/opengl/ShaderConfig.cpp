/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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
#include <contour/helper.h>
#include <contour/opengl/ShaderConfig.h>
#include <contour/opengl/background_frag.h>
#include <contour/opengl/background_vert.h>
#include <contour/opengl/shared_defines_verbatim.h>
#include <contour/opengl/text_frag.h>
#include <contour/opengl/text_vert.h>

#include <iostream>
#include <string>

namespace contour::opengl
{

namespace
{
    template <size_t N>
    inline std::string s(std::array<uint8_t, N> const& data)
    {
        return std::string(reinterpret_cast<char const*>(data.data()), data.size());
    }
} // namespace

ShaderConfig defaultShaderConfig(ShaderClass _shaderClass)
{
    using namespace verbatim;

    auto const p = [](std::string code) -> std::string {
        return s(shared_defines) + "#line 1\n" + move(code);
    };

    switch (_shaderClass)
    {
    case ShaderClass::Background:
        return {
            p(s(background_vert)), p(s(background_frag)), "builtin.background.vert", "builtin.background.frag"
        };
    case ShaderClass::Text:
        return { p(s(text_vert)), p(s(text_frag)), "builtin.text.vert", "builtin.text.frag" };
    }

    throw std::invalid_argument(fmt::format("ShaderClass<{}>", static_cast<unsigned>(_shaderClass)));
}

std::unique_ptr<QOpenGLShaderProgram> createShader(ShaderConfig const& _shaderConfig)
{
    auto shader = std::make_unique<QOpenGLShaderProgram>();

    LOGSTORE(DisplayLog)("Loading vertex shader: {}", _shaderConfig.vertexShaderFileName);
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, _shaderConfig.vertexShader.c_str()))
    {
        errorlog()("Compiling vertex shader {} failed. {}",
                   _shaderConfig.vertexShaderFileName,
                   shader->log().toStdString());
        qDebug() << shader->log();
        return {};
    }

    LOGSTORE(DisplayLog)("Loading fragment shader: {}", _shaderConfig.fragmentShaderFileName);
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Fragment, _shaderConfig.fragmentShader.c_str()))
    {
        errorlog()("Compiling fragment shader {} failed. {}",
                   _shaderConfig.fragmentShaderFileName,
                   shader->log().toStdString());
        return {};
    }

    if (!shader->link())
    {
        errorlog()("Linking shaders {} & {} failed. {}",
                   _shaderConfig.vertexShaderFileName,
                   _shaderConfig.fragmentShaderFileName,
                   shader->log().toStdString());
        return {};
    }

    if (auto const logString = shader->log().toStdString(); !logString.empty())
        errorlog()("Shader log: {}", logString);

    return shader;
}

} // namespace contour::opengl
