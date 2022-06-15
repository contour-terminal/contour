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

#include <contour/opengl/Vertex.h>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QtMath>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>
#include <QtGui/QVector2D>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGL/QOpenGLBuffer>
    #include <QtOpenGL/QOpenGLFramebufferObject>
    #include <QtOpenGL/QOpenGLFunctions_3_3_Core>
    #include <QtOpenGL/QOpenGLShaderProgram>
    #include <QtOpenGL/QOpenGLTexture>
    #include <QtOpenGL/QOpenGLVertexArrayObject>
#else
    #include <QtGui/QOpenGLBuffer>
    #include <QtGui/QOpenGLFramebufferObject>
    #include <QtGui/QOpenGLFunctions_3_3_Core>
    #include <QtGui/QOpenGLShaderProgram>
    #include <QtGui/QOpenGLTexture>
    #include <QtGui/QOpenGLVertexArrayObject>
#endif

namespace contour::opengl
{

// Dual Kawase Blur (GDC 2015)
//
// The implementation is heavily based on a implementation from
//      https://github.com/alex47/Dual-Kawase-Blur  (GPL-3)
// Which seems to be taken from KDE Window manager, using the same blur algorithm.
class Blur: protected QOpenGLFunctions_3_3_Core
{
  public:
    Blur();
    ~Blur() override;

    QImage blurDualKawase(QImage imageToBlur, int offset, int iterations);
    QImage blurGaussian(QImage imageToBlur);

    float getGPUTime();
    float getCPUTime();

  private:
    void renderToFBO(QOpenGLFramebufferObject* targetFBO, GLuint sourceTexture, QOpenGLShaderProgram* shader);
    void initFBOTextures();

    //.
    QOpenGLContext* m_context;
    QOffscreenSurface* m_surface;
    QOpenGLShaderProgram* m_gaussianBlur = nullptr;
    QOpenGLShaderProgram* m_shaderKawaseUp = nullptr;
    QOpenGLShaderProgram* m_shaderKawaseDown = nullptr;
    //.

    QVector<QOpenGLFramebufferObject*> m_FBO_vector;
    QOpenGLTexture* m_textureToBlur = nullptr;

    QOpenGLVertexArrayObject m_VertexArrayObject;
    QOpenGLBuffer m_vertexBuffer;

    int m_iterations = -1;
    QImage m_imageToBlur;

    // GPU timer
    GLuint64 GPUtimerElapsedTime {};

    // CPU timer
    QElapsedTimer CPUTimer;
    quint64 CPUTimerElapsedTime {};
};

} // namespace contour::opengl
