#pragma once

#include <string>

#include <GL/glew.h>
#include <glm/matrix.hpp>

class Shader
{
public:
    operator unsigned int () const noexcept { return id_; }

    Shader(std::string const& vertexCode, std::string const& fragmentCode, std::string const& geometryCode = "");
    ~Shader();

    void use();

    void setBool(const std::string& _name, bool _value) const;
    void setInt(const std::string& _name, int _value) const;
    void setFloat(const std::string& _name, float _value) const;

    void setVec2(const std::string& _name, const glm::vec2& _value) const;
    void setVec2(const std::string& _name, float _x, float _y) const;

    void setVec3(const std::string& _name, const glm::vec3& _value) const;
    void setVec3(const std::string& _name, float _x, float _y, float z) const;

    void setVec4(const std::string& _name, const glm::vec4& _value) const;
    void setVec4(const std::string& _name, float _x, float _y, float _z, float _w);

    void setMat2(const std::string& _name, const glm::mat2& _mat) const;
    void setMat3(const std::string& _name, const glm::mat3& _mat) const;
    void setMat4(const std::string& _name, const glm::mat4& _mat) const;

private:
    void checkCompileErrors(GLuint _shader, std::string _type);

private:
    unsigned int id_{};
};
