/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include "Shader.h"

#include <string>
#include <sstream>
#include <iostream>

#include <GL/glew.h>
#include <glm/matrix.hpp>

using namespace std;

Shader::Shader(string const& vertexCode, string const& fragmentCode, string const& geometryCode)
{
    const char* vShaderCode = vertexCode.c_str();
    const char* fShaderCode = fragmentCode.c_str();

    // vertex shader
    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    // fragment Shader
    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    // if geometry shader is given, compile geometry shader
    unsigned int geometry{};
    if (!geometryCode.empty())
    {
        const char* gShaderCode = geometryCode.c_str();
        geometry = glCreateShader(GL_GEOMETRY_SHADER);
        glShaderSource(geometry, 1, &gShaderCode, nullptr);
        glCompileShader(geometry);
        checkCompileErrors(geometry, "GEOMETRY");
    }

    // shader Program
    id_ = glCreateProgram();
    glAttachShader(id_, vertex);
    glAttachShader(id_, fragment);

    if (!geometryCode.empty())
        glAttachShader(id_, geometry);

    glLinkProgram(id_);
    checkCompileErrors(id_, "PROGRAM");

    // delete the shaders as they're linked into our program now and no longer necessery
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!geometryCode.empty())
        glDeleteShader(geometry);
}

Shader::~Shader()
{
    if (id_)
    {
        glDeleteProgram(id_);
        id_ = 0;
    }
}

void Shader::use()
{
    glUseProgram(id_);
}

GLint Shader::uniformLocation(string const& _name) const
{
    return glGetUniformLocation(id_, _name.c_str());
}

GLint Shader::attributeLocation(string const& _name) const
{
    return glGetAttribLocation(id_, _name.c_str());
}

// utility uniform functions
// ------------------------------------------------------------------------
void Shader::setBool(const string& name, bool value) const
{
    glUniform1i(glGetUniformLocation(id_, name.c_str()), (int)value);
}
// ------------------------------------------------------------------------
void Shader::setInt(const string& name, int value) const
{
    glUniform1i(glGetUniformLocation(id_, name.c_str()), value);
}
// ------------------------------------------------------------------------
void Shader::setFloat(const string& name, float value) const
{
    glUniform1f(glGetUniformLocation(id_, name.c_str()), value);
}
// ------------------------------------------------------------------------
void Shader::setVec2(GLint _id, const glm::vec2& value) const
{
    glUniform2fv(_id, 1, &value[0]);
}

void Shader::setVec2(const string& name, const glm::vec2& value) const
{
    glUniform2fv(glGetUniformLocation(id_, name.c_str()), 1, &value[0]);
}

void Shader::setVec3(GLint _id, const glm::vec3& value) const
{
    glUniform3fv(_id, 1, &value[0]);
}

void Shader::setVec3(const string& name, const glm::vec3& value) const
{
    glUniform3fv(glGetUniformLocation(id_, name.c_str()), 1, &value[0]);
}

void Shader::setVec4(GLint _id, const glm::vec4& value) const
{
    glUniform4fv(_id, 1, &value[0]);
}

void Shader::setVec4(const string& name, const glm::vec4& value) const
{
    glUniform4fv(glGetUniformLocation(id_, name.c_str()), 1, &value[0]);
}

void Shader::setMat2(const string& name, const glm::mat2& mat) const
{
    glUniformMatrix2fv(glGetUniformLocation(id_, name.c_str()), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat3(const string& name, const glm::mat3& mat) const
{
    glUniformMatrix3fv(glGetUniformLocation(id_, name.c_str()), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat4(const string& name, const glm::mat4& mat) const
{
    glUniformMatrix4fv(glGetUniformLocation(id_, name.c_str()), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat4(GLint _id, const glm::mat4& mat) const
{
    glUniformMatrix4fv(_id, 1, GL_FALSE, &mat[0][0]);
}

void Shader::checkCompileErrors(GLuint shader, string type)
{
    GLint success;
    GLchar infoLog[1024];
    if (type != "PROGRAM")
    {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
            stringstream sstr;
            sstr << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n" << infoLog << "\n -- --------------------------------------------------- -- " << endl;
            cerr << sstr.str();
            throw runtime_error{ sstr.str() };
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            stringstream sstr;
            sstr << "ERROR::PROGRAM_LINKING_ERROR of type: " << type << "\n" << infoLog << "\n -- --------------------------------------------------- -- " << endl;
            cerr << sstr.str();
            throw runtime_error{ sstr.str() };
        }
    }
}
