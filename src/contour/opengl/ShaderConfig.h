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

#include <fmt/format.h>

#include <QtCore/QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGL/QOpenGLShaderProgram>
#else
    #include <QtGui/QOpenGLShaderProgram>
#endif

#include <memory>
#include <stdexcept>
#include <string>

namespace contour::opengl
{

enum class ShaderClass
{
    BackgroundImage,
    Background,
    Text
};

struct ShaderConfig
{
    std::string vertexShader;
    std::string fragmentShader;
    std::string vertexShaderFileName;
    std::string fragmentShaderFileName;
};

inline std::string to_string(ShaderClass _shaderClass)
{
    switch (_shaderClass)
    {
    case ShaderClass::BackgroundImage: return "BackgroundImage";
    case ShaderClass::Background: return "background";
    case ShaderClass::Text: return "text";
    }

    throw std::invalid_argument(fmt::format("ShaderClass<{}>", static_cast<unsigned>(_shaderClass)));
}

ShaderConfig defaultShaderConfig(ShaderClass _shaderClass);

std::unique_ptr<QOpenGLShaderProgram> createShader(ShaderConfig const& _shaderConfig);

} // namespace contour::opengl
