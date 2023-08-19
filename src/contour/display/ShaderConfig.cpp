// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ShaderConfig.h>
#include <contour/helper.h>

#include <QtCore/QFile>
#include <QtGui/QOpenGLContext>

#include <iostream>
#include <string>
#include <string_view>
#include <tuple>

using std::get;
using std::holds_alternative;
using std::string;
using std::tuple;
using namespace std::string_literals;

namespace contour::display
{

auto const ShaderLog = logstore::category("gui.shader", "Logs shader configuration");

namespace
{
    template <size_t N>
    inline std::string s(std::array<uint8_t, N> const& data)
    {
        return std::string(reinterpret_cast<char const*>(data.data()), data.size());
    }
} // namespace

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

#if !defined(NDEBUG)
    format.setOption(QSurfaceFormat::DebugContext);
#endif

    return format;
}

ShaderConfig builtinShaderConfig(ShaderClass shaderClass)
{
    using namespace std::string_view_literals;
    auto const makeConfig = [](ShaderClass shaderClass) -> ShaderConfig {
        auto const makeSource = [](QString const& filename) -> ShaderSource {
            QFile sharedDefinesFile(":/contour/vtrasterizer/shared_defines.h");
            sharedDefinesFile.open(QFile::ReadOnly);
            Require(sharedDefinesFile.isOpen());
            auto const sharedDefines = sharedDefinesFile.readAll().toStdString() + "\n#line 1\n";
            auto const versionHeader = fmt::format("#version {}\n", useOpenGLES() ? "300 es" : "330");

            auto const shaderFilePath = ":/contour/display/shaders/" + filename;

            QFile file(shaderFilePath);
            file.open(QFile::ReadOnly);
            Require(file.isOpen());
            auto const fileContents = file.readAll().toStdString();

            auto const fileHeader = versionHeader + sharedDefines;
            return ShaderSource { shaderFilePath, QString::fromStdString(fileHeader + fileContents) };
        };
        QString basename = QString::fromStdString(to_string(shaderClass));
        return ShaderConfig { makeSource(basename + ".vert"), makeSource(basename + ".frag") };
    };
    return makeConfig(shaderClass);
}

std::unique_ptr<QOpenGLShaderProgram> createShader(ShaderConfig const& _shaderConfig)
{
    auto shader = std::make_unique<QOpenGLShaderProgram>();

    auto extractShaderSource = [](ShaderSource const& source) -> tuple<string, string> {
        return { source.location.toStdString(), source.contents.toStdString() };
    };

    auto [vertexLocation, vertexSource] = extractShaderSource(_shaderConfig.vertexShader);
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource.c_str()))
    {
        errorLog()("Compiling vertex shader {} failed.", vertexLocation);
        errorLog()("Shader source: {}", vertexSource);
        errorLog()("Shader log: {}", shader->log().toStdString());
        qDebug() << shader->log();
        return {};
    }

    auto [fragmentLocation, fragmentSource] = extractShaderSource(_shaderConfig.fragmentShader);
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource.c_str()))
    {
        errorLog()("Compiling fragment shader {} failed. {}", fragmentLocation, shader->log().toStdString());
        qDebug() << shader->log();
        return {};
    }

    if (!shader->link())
    {
        errorLog()("Linking shaders {} & {} failed. {}",
                   vertexLocation,
                   fragmentLocation,
                   shader->log().toStdString());
        return {};
    }

    if (auto const logString = shader->log().toStdString(); !logString.empty())
        ShaderLog()("{}", logString);

    Guarantee(shader->isLinked());
    return shader;
}

} // namespace contour::display
