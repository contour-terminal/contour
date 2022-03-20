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
#include <contour/helper.h>
#include <contour/opengl/ShaderConfig.h>

#include <QtCore/QFile>

#include <iostream>
#include <string>
#include <tuple>

using std::get;
using std::holds_alternative;
using std::string;
using std::tuple;
using namespace std::string_literals;

namespace contour::opengl
{

namespace
{
    template <size_t N>
    inline std::string s(std::array<uint8_t, N> const& data)
    {
        return std::string(reinterpret_cast<char const*>(data.data()), data.size());
    }
} // namespace

ShaderConfig builtinShaderConfig(ShaderClass shaderClass)
{
    auto const makeConfig = [](ShaderClass shaderClass) -> ShaderConfig {
        auto const makeSource = [](QString const& filename) -> ShaderSource {
            QFile sharedDefinesFile(":/contour/terminal_renderer/shared_defines.h");
            sharedDefinesFile.open(QFile::ReadOnly);
            Require(sharedDefinesFile.isOpen());
            auto const sharedDefines = sharedDefinesFile.readAll().toStdString() + "\n#line 1\n";

            auto const versionHeader = "#version 330\n";

            auto const shaderFilePath = ":/contour/opengl/shaders/" + filename;

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
    DisplayLog()("Loading vertex shader: {}", vertexLocation);
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource.c_str()))
    {
        errorlog()("Compiling vertex shader {} failed. {}", vertexLocation, shader->log().toStdString());
        qDebug() << shader->log();
        return {};
    }

    auto [fragmentLocation, fragmentSource] = extractShaderSource(_shaderConfig.fragmentShader);
    DisplayLog()("Loading fragment shader: {}", fragmentLocation);
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource.c_str()))
    {
        errorlog()("Compiling fragment shader {} failed. {}", fragmentLocation, shader->log().toStdString());
        qDebug() << shader->log();
        return {};
    }

    if (!shader->link())
    {
        errorlog()("Linking shaders {} & {} failed. {}",
                   vertexLocation,
                   fragmentLocation,
                   shader->log().toStdString());
        return {};
    }

    if (auto const logString = shader->log().toStdString(); !logString.empty())
        errorlog()("Shader log: {}", logString);

    return shader;
}

} // namespace contour::opengl
