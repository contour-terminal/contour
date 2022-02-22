#pragma once

#include <contour/opengl/Vertex.h>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QtMath>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLBuffer>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFramebufferObject>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtGui/QOpenGLShaderProgram>
#include <QtGui/QOpenGLTexture>
#include <QtGui/QOpenGLVertexArrayObject>
#include <QtGui/QVector2D>

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
    ~Blur();

    QImage blurDualKawase(QImage imageToBlur, int offset, int iterations);
    QImage blurGaussian(QImage imageToBlur);

    float getGPUTime();
    float getCPUTime();

  private:
    void renderToFBO(QOpenGLFramebufferObject* targetFBO, GLuint sourceTexture, QOpenGLShaderProgram* shader);
    void initFBOTextures();

    QOpenGLShaderProgram* m_shaderKawaseUp = nullptr;
    QOpenGLShaderProgram* m_shaderKawaseDown = nullptr;
    QOpenGLShaderProgram* m_gaussianBlur = nullptr;

    QVector<QOpenGLFramebufferObject*> m_FBO_vector;
    QOpenGLTexture* m_textureToBlur;

    QOpenGLVertexArrayObject m_VertexArrayObject;
    QOpenGLBuffer m_vertexBuffer;

    QOffscreenSurface* m_surface;
    QOpenGLContext* m_context;

    int m_iterations;
    QImage m_imageToBlur;

    // GPU timer
    GLuint64 GPUtimerElapsedTime;

    // CPU timer
    QElapsedTimer CPUTimer;
    quint64 CPUTimerElapsedTime;
};

} // namespace contour::opengl
