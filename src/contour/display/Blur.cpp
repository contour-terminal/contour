// SPDX-License-Identifier: Apache-2.0
#include <contour/display/Blur.h>
#include <contour/display/ShaderConfig.h>
#include <contour/helper.h>

#include <crispy/assert.h>

#include <QtCore/QFile>

#include <type_traits>

// NB: Cannot be enabled as it's not available on OpenGL ES, so it seems.
// #define CONTOUR_GPU_TIMERS 1

namespace contour::display
{

namespace
{
    Vertex const sgVertexes[] = { Vertex(QVector3D(1.0f, 1.0f, 1.0f)),
                                  Vertex(QVector3D(-1.0f, 1.0f, 1.0f)),
                                  Vertex(QVector3D(-1.0f, -1.0f, 1.0f)),
                                  Vertex(QVector3D(1.0f, -1.0f, 1.0f)) };

    QString loadShaderSource(QString const& shaderFilePath)
    {
        displayLog()("Blur: Loading shader source {}", shaderFilePath.toStdString());
        auto const versionHeader =
            QString::fromStdString(fmt::format("#version {}\n", useOpenGLES() ? "300 es" : "330"));

        QFile file(shaderFilePath);
        file.open(QFile::ReadOnly);
        Require(file.isOpen());
        auto const fileContents = file.readAll();
        return versionHeader + "#line 1\n" + fileContents;
    }

} // namespace

Blur::Blur():
    _context { new QOpenGLContext() },
    _surface { new QOffscreenSurface() },
    _gaussianBlur { new QOpenGLShaderProgram() },
    _shaderKawaseUp { new QOpenGLShaderProgram() },
    _shaderKawaseDown { new QOpenGLShaderProgram() }
{
    auto const contextCreationSucceed = _context->create();
    Require(contextCreationSucceed);
    Require(_context->isValid());
    Require(_context->format().renderableType() == QSurfaceFormat::RenderableType::OpenGL
            || _context->format().renderableType() == QSurfaceFormat::RenderableType::OpenGLES);

    _surface->create();
    Require(_surface->format().renderableType() == QSurfaceFormat::RenderableType::OpenGL
            || _surface->format().renderableType() == QSurfaceFormat::RenderableType::OpenGLES);

    auto const makeCurrentSucceed = _context->makeCurrent(_surface);
    Require(makeCurrentSucceed);

    Require(_context->isValid());

    initializeOpenGLFunctions();

    // clang-format off

    // {{{ gaussian
    _gaussianBlur->addShaderFromSourceCode(QOpenGLShader::Vertex, loadShaderSource(":/contour/display/shaders/simple.vert"));
    _gaussianBlur->addShaderFromSourceCode(QOpenGLShader::Fragment, loadShaderSource(":/contour/display/shaders/blur_gaussian.frag"));
    _gaussianBlur->link();
    Guarantee(_gaussianBlur->isLinked());
    // }}}

    // {{{ dual kawase
    _shaderKawaseUp->addShaderFromSourceCode(QOpenGLShader::Vertex, loadShaderSource(":/contour/display/shaders/simple.vert"));
    _shaderKawaseUp->addShaderFromSourceCode(QOpenGLShader::Fragment, loadShaderSource(":/contour/display/shaders/dual_kawase_up.frag"));
    _shaderKawaseUp->link();

    _shaderKawaseDown->addShaderFromSourceCode(QOpenGLShader::Vertex, loadShaderSource(":/contour/display/shaders/simple.vert"));
    _shaderKawaseDown->addShaderFromSourceCode(QOpenGLShader::Fragment, loadShaderSource(":/contour/display/shaders/dual_kawase_down.frag"));
    _shaderKawaseDown->link();
    // }}}

    // clang-format on

    _vertexBuffer.create();
    _vertexBuffer.bind();
    _vertexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    _vertexBuffer.allocate(sgVertexes, sizeof(sgVertexes));

    _vertexArrayObject.create();
    _vertexArrayObject.bind();

    _shaderKawaseUp->enableAttributeArray(0);
    _shaderKawaseUp->setAttributeBuffer(
        0, GL_FLOAT, Vertex::positionOffset(), Vertex::PositionTupleSize, Vertex::stride());

    _shaderKawaseDown->enableAttributeArray(0);
    _shaderKawaseDown->setAttributeBuffer(
        0, GL_FLOAT, Vertex::positionOffset(), Vertex::PositionTupleSize, Vertex::stride());
}

Blur::~Blur()
{
    _context->makeCurrent(_surface);

    delete _shaderKawaseUp;
    delete _shaderKawaseDown;
    delete _gaussianBlur;

    for (auto& i: _vectorFBO)
        delete i;

    delete _textureToBlur;
}

QImage Blur::blurGaussian(QImage imageToBlur)
{
    _context->makeCurrent(_surface);
    Require(_context->isValid());

    if (imageToBlur != _imageToBlur)
    {
        _iterations = 1;
        _imageToBlur = std::move(imageToBlur);
        initFBOTextures();
        Require(_gaussianBlur->isLinked());

        _gaussianBlur->bind();
        _gaussianBlur->setUniformValue(
            "u_textureResolution",
            QVector2D((float) _imageToBlur.size().width(), (float) _imageToBlur.size().height()));
    }

// Start the GPU timer
#if defined(CONTOUR_GPU_TIMERS)
    GLuint gpuTimerQuery {};
    glGenQueries(1, &gpuTimerQuery);
    glBeginQuery(GL_TIME_ELAPSED, gpuTimerQuery);
#endif
    _timerCPU.start();

    renderToFBO(_vectorFBO[0], _textureToBlur->textureId(), _gaussianBlur);

    _timerCPUElapsedTime = (quint64) _timerCPU.nsecsElapsed();

#if defined(CONTOUR_GPU_TIMERS)
    glEndQuery(GL_TIME_ELAPSED);
    GLint GPUTimerAvailable = 0;
    while (!GPUTimerAvailable)
        glGetQueryObjectiv(gpuTimerQuery, GL_QUERY_RESULT_AVAILABLE, &_GPUTimerAvailable);

    glGetQueryObjectui64v(gpuTimerQuery, GL_QUERY_RESULT, &_timerGPUElapsedTime);
    glDeleteQueries(1, &gpuTimerQuery);
#endif

    auto image = _vectorFBO[0]->toImage();
    _context->doneCurrent();

#if defined(CONTOUR_GPU_TIMERS)
    displayLog()("Blur: Gaussian run performance: {:.3}s CPU, {:.3}s GPU.", getCPUTime(), getGPUTime());
#else
    displayLog()("Blur: Gaussian run performance: {:.3}s CPU.", getCPUTime());
#endif
    return image;
}

QImage Blur::blurDualKawase(QImage imageToBlur, int offset, int iterations)
{
    _context->makeCurrent(_surface);

    // Check to avoid unnecessary texture reallocation
    if (iterations != _iterations || imageToBlur != _imageToBlur)
    {
        _iterations = iterations;
        _imageToBlur = std::move(imageToBlur);

        initFBOTextures();
    }

// Don't record the texture and FBO allocation time

// Start the GPU timer
#if defined(CONTOUR_GPU_TIMERS)
    GLuint gpuTimerQuery {};
    glGenQueries(1, &gpuTimerQuery);
    glBeginQuery(GL_TIME_ELAPSED, gpuTimerQuery);
#endif

    _timerCPU.start();

    _shaderKawaseDown->setUniformValue("u_offset", QVector2D((float) offset, (float) offset));
    _shaderKawaseUp->setUniformValue("u_offset", QVector2D((float) offset, (float) offset));

    // Initial downsample
    // We only need this helper texture because we can't put a QImage into the texture of a
    // QOpenGLFramebufferObject Otherwise we would skip this and start the downsampling from _vectorFBO[0]
    // instead of _vectorFBO[1]
    renderToFBO(_vectorFBO[1], _textureToBlur->textureId(), _shaderKawaseDown);

    // Downsample
    for (int i = 1; i < iterations; i++)
        renderToFBO(_vectorFBO[i + 1], _vectorFBO[i]->texture(), _shaderKawaseDown);

    // Upsample
    for (int i = iterations; i > 0; i--)
        renderToFBO(_vectorFBO[i - 1], _vectorFBO[i]->texture(), _shaderKawaseUp);
    // --------------- blur end ---------------

    // Get the CPU timer result
    _timerCPUElapsedTime = (quint64) _timerCPU.nsecsElapsed();

// Get the GPU timer result
#if defined(CONTOUR_GPU_TIMERS)
    glEndQuery(GL_TIME_ELAPSED);
    GLint GPUTimerAvailable = 0;
    while (!GPUTimerAvailable)
        glGetQueryObjectiv(gpuTimerQuery, GL_QUERY_RESULT_AVAILABLE, &GPUTimerAvailable);

    glGetQueryObjectui64v(gpuTimerQuery, GL_QUERY_RESULT, &_timerGPUElapsedTime);
    glDeleteQueries(1, &gpuTimerQuery);
#endif

    auto image = _vectorFBO[0]->toImage();
    _context->doneCurrent();
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
    CHECKED_GL(glDrawArrays(GL_TRIANGLE_FAN, 0, sizeof(sgVertexes) / sizeof(sgVertexes[0])));
}

void Blur::initFBOTextures()
{
    for (auto& i: _vectorFBO)
        delete i;

    _vectorFBO.clear();
    _vectorFBO.append(new QOpenGLFramebufferObject(
        _imageToBlur.size(), QOpenGLFramebufferObject::CombinedDepthStencil, GL_TEXTURE_2D));

    for (int i = 1; i <= _iterations; i++)
    {
        _vectorFBO.append(new QOpenGLFramebufferObject(
            _imageToBlur.size() / qPow(2, i), QOpenGLFramebufferObject::CombinedDepthStencil, GL_TEXTURE_2D));

        CHECKED_GL(glBindTexture(GL_TEXTURE_2D, _vectorFBO.last()->texture()));
        CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    }

    delete _textureToBlur;

    _textureToBlur = new QOpenGLTexture(_imageToBlur.mirrored(), QOpenGLTexture::DontGenerateMipMaps);
    _textureToBlur->setWrapMode(QOpenGLTexture::ClampToEdge);
    _textureToBlur->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
}

float Blur::getGPUTime() const noexcept
{
    float const gpuTime = float(_timerGPUElapsedTime) / 1000000.f;
    return roundf(gpuTime * 1000) / 1000;
}

float Blur::getCPUTime() const noexcept
{
    float const cpuTime = float(_timerCPUElapsedTime) / 1000000.f;
    return roundf(cpuTime * 1000) / 1000;
}

} // namespace contour::display
