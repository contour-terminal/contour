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
#include <contour/display/Blur.h>
#include <contour/display/ShaderConfig.h>
#include <contour/helper.h>

#include <crispy/assert.h>

#include <QtCore/QFile>

#include <type_traits>

using std::move;

// NB: Cannot be enabled as it's not available on OpenGL ES, so it seems.
// #define CONTOUR_GPU_TIMERS 1

namespace contour::display
{

static const Vertex sg_vertexes[] = { Vertex(QVector3D(1.0f, 1.0f, 1.0f)),
                                      Vertex(QVector3D(-1.0f, 1.0f, 1.0f)),
                                      Vertex(QVector3D(-1.0f, -1.0f, 1.0f)),
                                      Vertex(QVector3D(1.0f, -1.0f, 1.0f)) };

QString loadShaderSource(QString const& shaderFilePath)
{
    DisplayLog()("Blur: Loading shader source {}", shaderFilePath.toStdString());
    auto const versionHeader =
        QString::fromStdString(fmt::format("#version {}\n", useOpenGLES() ? "300 es" : "330"));

    QFile file(shaderFilePath);
    file.open(QFile::ReadOnly);
    Require(file.isOpen());
    auto const fileContents = file.readAll();
    return versionHeader + "#line 1\n" + fileContents;
}

Blur::Blur():
    m_context { new QOpenGLContext() },
    m_surface { new QOffscreenSurface() },
    m_gaussianBlur { new QOpenGLShaderProgram() },
    m_shaderKawaseUp { new QOpenGLShaderProgram() },
    m_shaderKawaseDown { new QOpenGLShaderProgram() }
{
    auto const contextCreationSucceed = m_context->create();
    Require(contextCreationSucceed);
    Require(m_context->isValid());
    Require(m_context->format().renderableType() == QSurfaceFormat::RenderableType::OpenGL
            || m_context->format().renderableType() == QSurfaceFormat::RenderableType::OpenGLES);

    m_surface->create();
    Require(m_surface->format().renderableType() == QSurfaceFormat::RenderableType::OpenGL
            || m_surface->format().renderableType() == QSurfaceFormat::RenderableType::OpenGLES);

    auto const makeCurrentSucceed = m_context->makeCurrent(m_surface);
    Require(makeCurrentSucceed);

    Require(m_context->isValid());

    initializeOpenGLFunctions();

    // clang-format off

    // {{{ gaussian
    m_gaussianBlur->addShaderFromSourceCode(QOpenGLShader::Vertex, loadShaderSource(":/contour/display/shaders/simple.vert"));
    m_gaussianBlur->addShaderFromSourceCode(QOpenGLShader::Fragment, loadShaderSource(":/contour/display/shaders/blur_gaussian.frag"));
    m_gaussianBlur->link();
    Guarantee(m_gaussianBlur->isLinked());
    // }}}

    // {{{ dual kawase
    m_shaderKawaseUp->addShaderFromSourceCode(QOpenGLShader::Vertex, loadShaderSource(":/contour/display/shaders/simple.vert"));
    m_shaderKawaseUp->addShaderFromSourceCode(QOpenGLShader::Fragment, loadShaderSource(":/contour/display/shaders/dual_kawase_up.frag"));
    m_shaderKawaseUp->link();

    m_shaderKawaseDown->addShaderFromSourceCode(QOpenGLShader::Vertex, loadShaderSource(":/contour/display/shaders/simple.vert"));
    m_shaderKawaseDown->addShaderFromSourceCode(QOpenGLShader::Fragment, loadShaderSource(":/contour/display/shaders/dual_kawase_down.frag"));
    m_shaderKawaseDown->link();
    // }}}

    // clang-format on

    m_vertexBuffer.create();
    m_vertexBuffer.bind();
    m_vertexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    m_vertexBuffer.allocate(sg_vertexes, sizeof(sg_vertexes));

    m_VertexArrayObject.create();
    m_VertexArrayObject.bind();

    m_shaderKawaseUp->enableAttributeArray(0);
    m_shaderKawaseUp->setAttributeBuffer(
        0, GL_FLOAT, Vertex::positionOffset(), Vertex::PositionTupleSize, Vertex::stride());

    m_shaderKawaseDown->enableAttributeArray(0);
    m_shaderKawaseDown->setAttributeBuffer(
        0, GL_FLOAT, Vertex::positionOffset(), Vertex::PositionTupleSize, Vertex::stride());
}

Blur::~Blur()
{
    m_context->makeCurrent(m_surface);

    delete m_shaderKawaseUp;
    delete m_shaderKawaseDown;
    delete m_gaussianBlur;

    for (auto& i: m_FBO_vector)
        delete i;

    delete m_textureToBlur;
}

QImage Blur::blurGaussian(QImage imageToBlur)
{
    m_context->makeCurrent(m_surface);
    Require(m_context->isValid());

    if (imageToBlur != m_imageToBlur)
    {
        m_iterations = 1;
        m_imageToBlur = move(imageToBlur);
        initFBOTextures();
        Require(m_gaussianBlur->isLinked());

        m_gaussianBlur->bind();
        m_gaussianBlur->setUniformValue(
            "u_textureResolution",
            QVector2D((float) m_imageToBlur.size().width(), (float) m_imageToBlur.size().height()));
    }

// Start the GPU timer
#if defined(CONTOUR_GPU_TIMERS)
    GLuint gpuTimerQuery {};
    glGenQueries(1, &gpuTimerQuery);
    glBeginQuery(GL_TIME_ELAPSED, gpuTimerQuery);
#endif
    CPUTimer.start();

    renderToFBO(m_FBO_vector[0], m_textureToBlur->textureId(), m_gaussianBlur);

    CPUTimerElapsedTime = (quint64) CPUTimer.nsecsElapsed();

#if defined(CONTOUR_GPU_TIMERS)
    glEndQuery(GL_TIME_ELAPSED);
    GLint GPUTimerAvailable = 0;
    while (!GPUTimerAvailable)
        glGetQueryObjectiv(gpuTimerQuery, GL_QUERY_RESULT_AVAILABLE, &GPUTimerAvailable);

    glGetQueryObjectui64v(gpuTimerQuery, GL_QUERY_RESULT, &GPUtimerElapsedTime);
    glDeleteQueries(1, &gpuTimerQuery);
#endif

    auto image = m_FBO_vector[0]->toImage();
    m_context->doneCurrent();

#if defined(CONTOUR_GPU_TIMERS)
    DisplayLog()("Blur: Gaussian run performance: {:.3}s CPU, {:.3}s GPU.", getCPUTime(), getGPUTime());
#else
    DisplayLog()("Blur: Gaussian run performance: {:.3}s CPU.", getCPUTime());
#endif
    return image;
}

QImage Blur::blurDualKawase(QImage imageToBlur, int offset, int iterations)
{
    m_context->makeCurrent(m_surface);

    // Check to avoid unnecessary texture reallocation
    if (iterations != m_iterations || imageToBlur != m_imageToBlur)
    {
        m_iterations = iterations;
        m_imageToBlur = move(imageToBlur);

        initFBOTextures();
    }

// Don't record the texture and FBO allocation time

// Start the GPU timer
#if defined(CONTOUR_GPU_TIMERS)
    GLuint gpuTimerQuery {};
    glGenQueries(1, &gpuTimerQuery);
    glBeginQuery(GL_TIME_ELAPSED, gpuTimerQuery);
#endif

    CPUTimer.start();

    m_shaderKawaseDown->setUniformValue("u_offset", QVector2D((float) offset, (float) offset));
    m_shaderKawaseUp->setUniformValue("u_offset", QVector2D((float) offset, (float) offset));

    // Initial downsample
    // We only need this helper texture because we can't put a QImage into the texture of a
    // QOpenGLFramebufferObject Otherwise we would skip this and start the downsampling from m_FBO_vector[0]
    // instead of m_FBO_vector[1]
    renderToFBO(m_FBO_vector[1], m_textureToBlur->textureId(), m_shaderKawaseDown);

    // Downsample
    for (int i = 1; i < iterations; i++)
        renderToFBO(m_FBO_vector[i + 1], m_FBO_vector[i]->texture(), m_shaderKawaseDown);

    // Upsample
    for (int i = iterations; i > 0; i--)
        renderToFBO(m_FBO_vector[i - 1], m_FBO_vector[i]->texture(), m_shaderKawaseUp);
    // --------------- blur end ---------------

    // Get the CPU timer result
    CPUTimerElapsedTime = (quint64) CPUTimer.nsecsElapsed();

// Get the GPU timer result
#if defined(CONTOUR_GPU_TIMERS)
    glEndQuery(GL_TIME_ELAPSED);
    GLint GPUTimerAvailable = 0;
    while (!GPUTimerAvailable)
        glGetQueryObjectiv(gpuTimerQuery, GL_QUERY_RESULT_AVAILABLE, &GPUTimerAvailable);

    glGetQueryObjectui64v(gpuTimerQuery, GL_QUERY_RESULT, &GPUtimerElapsedTime);
    glDeleteQueries(1, &gpuTimerQuery);
#endif

    auto image = m_FBO_vector[0]->toImage();
    m_context->doneCurrent();
    return image;
}

void Blur::renderToFBO(QOpenGLFramebufferObject* targetFBO,
                       GLuint sourceTexture,
                       QOpenGLShaderProgram* shader)
{
    targetFBO->bind();
    CHECKED_GL(glBindTexture(GL_TEXTURE_2D, sourceTexture));
    shader->bind();

    shader->setUniformValue("u_viewportResolution",
                            QVector2D((float) targetFBO->size().width(), (float) targetFBO->size().height()));

    shader->setUniformValue(
        "u_halfpixel",
        QVector2D(0.5f / (float) targetFBO->size().width(), 0.5f / (float) targetFBO->size().height()));

    CHECKED_GL(glViewport(0, 0, targetFBO->size().width(), targetFBO->size().height()));
    CHECKED_GL(glDrawArrays(GL_TRIANGLE_FAN, 0, sizeof(sg_vertexes) / sizeof(sg_vertexes[0])));
}

void Blur::initFBOTextures()
{
    for (auto& i: m_FBO_vector)
        delete i;

    m_FBO_vector.clear();
    m_FBO_vector.append(new QOpenGLFramebufferObject(
        m_imageToBlur.size(), QOpenGLFramebufferObject::CombinedDepthStencil, GL_TEXTURE_2D));

    for (int i = 1; i <= m_iterations; i++)
    {
        m_FBO_vector.append(new QOpenGLFramebufferObject(m_imageToBlur.size() / qPow(2, i),
                                                         QOpenGLFramebufferObject::CombinedDepthStencil,
                                                         GL_TEXTURE_2D));

        CHECKED_GL(glBindTexture(GL_TEXTURE_2D, m_FBO_vector.last()->texture()));
        CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    }

    delete m_textureToBlur;

    m_textureToBlur = new QOpenGLTexture(m_imageToBlur.mirrored(), QOpenGLTexture::DontGenerateMipMaps);
    m_textureToBlur->setWrapMode(QOpenGLTexture::ClampToEdge);
    m_textureToBlur->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
}

float Blur::getGPUTime()
{
    float const gpuTime = float(GPUtimerElapsedTime) / 1000000.f;
    return roundf(gpuTime * 1000) / 1000;
}

float Blur::getCPUTime()
{
    float const cpuTime = float(CPUTimerElapsedTime) / 1000000.f;
    return roundf(cpuTime * 1000) / 1000;
}

} // namespace contour::display
