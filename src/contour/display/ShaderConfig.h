// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <fmt/format.h>

#include <QtCore/QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtGui/QSurfaceFormat>

    #include <QtOpenGL/QOpenGLShaderProgram>
#else
    #include <QtGui/QOpenGLShaderProgram>
    #include <QtGui/QSurfaceFormat>
#endif

#include <memory>
#include <stdexcept>
#include <string>

namespace contour::display
{

enum class ShaderClass
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
