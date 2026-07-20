// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ShaderConfig.h>

#include <QtGui/QOpenGLContext>

namespace contour::display
{

bool useOpenGLES() noexcept
{
    return QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;
}

QSurfaceFormat createSurfaceFormat()
{
    QSurfaceFormat format;

    if (useOpenGLES())
    {
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setVersion(3, 0);
    }
    else
    {
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
    }

    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setAlphaBufferSize(8);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);

#ifndef NDEBUG
    format.setOption(QSurfaceFormat::DebugContext);
#endif

    return format;
}

} // namespace contour::display
