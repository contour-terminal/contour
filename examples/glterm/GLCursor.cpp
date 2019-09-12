#include "GLCursor.h"

using namespace std;

GLCursor::GLCursor(glm::ivec2 _size, CursorShape _shape)
{
    // TODO: timer fuer blinking starten
}

void GLCursor::setShape(CursorShape _shape)
{
    // setzt nicht nur shape_ sondern auch gleich die QUAD vertices,
    // sodass sie das jeweilige shape formen
}

void GLCursor::setTransform(glm::mat4 _mat)
{
}

void GLCursor::resize(glm::ivec2 _size)
{
    }

void GLCursor::render(glm::mat4 transform)
{
    // kann alle shapes rendern, same code paths
}

