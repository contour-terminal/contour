// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <QtCore/QtGlobal>
#include <QtGui/QSurfaceFormat>
#include <QtOpenGL/QOpenGLShaderProgram>

#include <memory>
#include <string>

namespace contour::display
{

enum class ShaderClass : uint8_t
{
    Background,
    Text
};

struct ShaderSource
{
    QString location;
    QString contents;
};

struct ShaderConfig
{
    ShaderSource vertexShader;
    ShaderSource fragmentShader;
};

bool useOpenGLES() noexcept;
QSurfaceFormat createSurfaceFormat();

inline std::string to_string(ShaderClass shaderClass)
{
    switch (shaderClass)
    {
        case ShaderClass::Background: return "background";
        case ShaderClass::Text: return "text";
    }

    crispy::unreachable();
}

ShaderConfig builtinShaderConfig(ShaderClass shaderClass);

std::unique_ptr<QOpenGLShaderProgram> createShader(ShaderConfig const& shaderConfig);

} // namespace contour::display
