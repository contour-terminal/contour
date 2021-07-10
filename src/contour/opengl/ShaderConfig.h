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
#pragma once

#include <crispy/debuglog.h>

#include <fmt/format.h>
#if defined(CONTOUR_BUILD_WITH_QT6)
    #include <QtOpenGL/QOpenGLShaderProgram>
#else
    #include <QtGui/QOpenGLShaderProgram>
#endif

#include <memory>
#include <string>
#include <stdexcept>

namespace terminal::renderer::opengl {

auto const inline OpenGLRendererTag = crispy::debugtag::make("renderer.opengl", "Logs OpenGL render target specific debugging information.");

enum class ShaderClass {
    Background,
    Text
};

struct ShaderConfig {
    std::string vertexShader;
    std::string fragmentShader;
    std::string vertexShaderFileName;
    std::string fragmentShaderFileName;
};

inline std::string to_string(ShaderClass _shaderClass)
{
    switch (_shaderClass)
    {
        case ShaderClass::Background:
            return "background";
        case ShaderClass::Text:
            return "text";
    }

    throw std::invalid_argument(fmt::format("ShaderClass<{}>", static_cast<unsigned>(_shaderClass)));
}

ShaderConfig defaultShaderConfig(ShaderClass _shaderClass);

std::unique_ptr<QOpenGLShaderProgram> createShader(ShaderConfig const& _shaderConfig);

} // namespace
