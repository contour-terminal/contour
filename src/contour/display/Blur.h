// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/Vertex.h>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QtMath>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>
#include <QtGui/QVector2D>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtGui/QOpenGLExtraFunctions>

    #include <QtOpenGL/QOpenGLBuffer>
    #include <QtOpenGL/QOpenGLFramebufferObject>
    #include <QtOpenGL/QOpenGLFunctions_3_3_Core>
    #include <QtOpenGL/QOpenGLShaderProgram>
    #include <QtOpenGL/QOpenGLTexture>
    #include <QtOpenGL/QOpenGLVertexArrayObject>
#else
    #include <QtGui/QOpenGLBuffer>
    #include <QtGui/QOpenGLExtraFunctions>
    #include <QtGui/QOpenGLFramebufferObject>
    #include <QtGui/QOpenGLFunctions_3_3_Core>
    #include <QtGui/QOpenGLShaderProgram>
    #include <QtGui/QOpenGLTexture>
    #include <QtGui/QOpenGLVertexArrayObject>
#endif

namespace contour::display
{

// Dual Kawase Blur (GDC 2015)
//
// The implementation is heavily based on a implementation from
//      https://github.com/alex47/Dual-Kawase-Blur  (GPL-3)
// Which seems to be taken from KDE Window manager, using the same blur algorithm.
class Blur: protected QOpenGLExtraFunctions
{
  public:
    Blur();
    ~Blur();

    QImage blurDualKawase(QImage imageToBlur, int offset, int iterations);
    QImage blurGaussian(QImage imageToBlur);

    [[nodiscard]] float getGPUTime() const noexcept;
    [[nodiscard]] float getCPUTime() const noexcept;

  private:
    void renderToFBO(QOpenGLFramebufferObject* targetFBO, GLuint sourceTexture, QOpenGLShaderProgram* shader);
    void initFBOTextures();

    //.
    QOpenGLContext* _context;
    QOffscreenSurface* _surface;
    QOpenGLShaderProgram* _gaussianBlur = nullptr;
    QOpenGLShaderProgram* _shaderKawaseUp = nullptr;
    QOpenGLShaderProgram* _shaderKawaseDown = nullptr;
    //.

    QVector<QOpenGLFramebufferObject*> _vectorFBO;
    QOpenGLTexture* _textureToBlur = nullptr;

    QOpenGLVertexArrayObject _vertexArrayObject;
    QOpenGLBuffer _vertexBuffer;

    int _iterations = -1;
    QImage _imageToBlur;

    // GPU timer
    GLuint64 _timerGPUElapsedTime {};

    // CPU timer
    QElapsedTimer _timerCPU;
    quint64 _timerCPUElapsedTime {};
};

} // namespace contour::display
