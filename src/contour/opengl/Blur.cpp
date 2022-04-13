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
#include <contour/opengl/Blur.h>

#include <type_traits>

using std::move;

namespace contour::opengl
{

static const Vertex sg_vertexes[] = { Vertex(QVector3D(1.0f, 1.0f, 1.0f)),
                                      Vertex(QVector3D(-1.0f, 1.0f, 1.0f)),
                                      Vertex(QVector3D(-1.0f, -1.0f, 1.0f)),
                                      Vertex(QVector3D(1.0f, -1.0f, 1.0f)) };

Blur::Blur():
    m_context { new QOpenGLContext() },
    m_surface { new QOffscreenSurface() },
    m_gaussianBlur { new QOpenGLShaderProgram() },
    m_shaderKawaseUp { new QOpenGLShaderProgram() },
    m_shaderKawaseDown { new QOpenGLShaderProgram() }
{
    m_context->setFormat(QSurfaceFormat::defaultFormat());
    m_context->create();

    m_surface->create();

    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    m_surface->setFormat(format);

    m_context->makeCurrent(m_surface);

    initializeOpenGLFunctions();

    // clang-format off

    // {{{ gaussian
    m_gaussianBlur->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/contour/opengl/shaders/simple.vert");
    m_gaussianBlur->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/contour/opengl/shaders/blur_gaussian.frag");
    m_gaussianBlur->link();
    // }}}

    // {{{ dual kawase
    m_shaderKawaseUp->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/contour/opengl/shaders/simple.vert");
    m_shaderKawaseUp->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/contour/opengl/shaders/dual_kawase_up.frag");
    m_shaderKawaseUp->link();

    m_shaderKawaseDown->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/contour/opengl/shaders/simple.vert");
    m_shaderKawaseDown->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/contour/opengl/shaders/dual_kawase_down.frag");
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

    m_textureToBlur = new QOpenGLTexture(QImage());
    m_textureToBlur->setWrapMode(QOpenGLTexture::ClampToEdge);
    m_textureToBlur->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);

    m_imageToBlur = QImage();
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

    if (imageToBlur != m_imageToBlur)
    {
        m_iterations = 1;
        m_imageToBlur = move(imageToBlur);
        initFBOTextures();

        m_gaussianBlur->bind();
        m_gaussianBlur->setUniformValue(
            "u_textureResolution",
            QVector2D((float) m_imageToBlur.size().width(), (float) m_imageToBlur.size().height()));
    }

    // Start the GPU timer
    GLuint gpuTimerQuery {};
    glGenQueries(1, &gpuTimerQuery);
    glBeginQuery(GL_TIME_ELAPSED, gpuTimerQuery);

    CPUTimer.start();
    renderToFBO(m_FBO_vector[0], m_textureToBlur->textureId(), m_gaussianBlur);
    CPUTimerElapsedTime = (quint64) CPUTimer.nsecsElapsed();
    glEndQuery(GL_TIME_ELAPSED);
    GLint GPUTimerAvailable = 0;
    while (!GPUTimerAvailable)
        glGetQueryObjectiv(gpuTimerQuery, GL_QUERY_RESULT_AVAILABLE, &GPUTimerAvailable);

    glGetQueryObjectui64v(gpuTimerQuery, GL_QUERY_RESULT, &GPUtimerElapsedTime);
    glDeleteQueries(1, &gpuTimerQuery);

    auto image = m_FBO_vector[0]->toImage();
    m_context->doneCurrent();
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
    GLuint gpuTimerQuery {};
    glGenQueries(1, &gpuTimerQuery);
    glBeginQuery(GL_TIME_ELAPSED, gpuTimerQuery);

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
    glEndQuery(GL_TIME_ELAPSED);
    GLint GPUTimerAvailable = 0;
    while (!GPUTimerAvailable)
        glGetQueryObjectiv(gpuTimerQuery, GL_QUERY_RESULT_AVAILABLE, &GPUTimerAvailable);

    glGetQueryObjectui64v(gpuTimerQuery, GL_QUERY_RESULT, &GPUtimerElapsedTime);
    glDeleteQueries(1, &gpuTimerQuery);

    auto image = m_FBO_vector[0]->toImage();
    m_context->doneCurrent();
    return image;
}

void Blur::renderToFBO(QOpenGLFramebufferObject* targetFBO,
                       GLuint sourceTexture,
                       QOpenGLShaderProgram* shader)
{
    targetFBO->bind();
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    shader->bind();

    shader->setUniformValue("u_viewportResolution",
                            QVector2D((float) targetFBO->size().width(), (float) targetFBO->size().height()));

    shader->setUniformValue(
        "u_halfpixel",
        QVector2D(0.5f / (float) targetFBO->size().width(), 0.5f / (float) targetFBO->size().height()));

    glViewport(0, 0, targetFBO->size().width(), targetFBO->size().height());
    glDrawArrays(GL_TRIANGLE_FAN, 0, sizeof(sg_vertexes) / sizeof(sg_vertexes[0]));
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

        glBindTexture(GL_TEXTURE_2D, m_FBO_vector.last()->texture());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
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

} // namespace contour::opengl
