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
#pragma once

#include <string>

#include <GL/glew.h>
#include <glm/matrix.hpp>

namespace terminal::view {

class Shader
{
public:
    operator unsigned int () const noexcept { return id_; }

    Shader(std::string const& vertexCode, std::string const& fragmentCode, std::string const& geometryCode = "");
    ~Shader();

    void use();

    GLint uniformLocation(std::string const& _name) const;
    GLint attributeLocation(std::string const& _name) const;

    void setBool(const std::string& _name, bool _value) const;
    void setInt(const std::string& _name, int _value) const;
    void setFloat(const std::string& _name, float _value) const;

    void setVec2(GLint _id, const glm::vec2& _value) const;
    void setVec2(const std::string& _name, const glm::vec2& _value) const;

    void setVec3(GLint _id, const glm::vec3& _value) const;
    void setVec3(const std::string& _name, const glm::vec3& _value) const;

    void setVec4(GLint _id, const glm::vec4& _value) const;
    void setVec4(const std::string& _name, const glm::vec4& _value) const;

    void setMat2(const std::string& _name, const glm::mat2& _mat) const;
    void setMat3(const std::string& _name, const glm::mat3& _mat) const;
    void setMat4(GLint _id, const glm::mat4& _mat) const;
    void setMat4(const std::string& _name, const glm::mat4& _mat) const;

private:
    void checkCompileErrors(GLuint _shader, std::string _type);

private:
    unsigned int id_{};
};

} // namespace terminal::view
