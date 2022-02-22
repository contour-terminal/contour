#pragma once

#include <QtCore/QTypeInfo>
#include <QtGui/QVector3D>

class Vertex
{
  public:
    Q_DECL_CONSTEXPR Vertex();
    Q_DECL_CONSTEXPR explicit Vertex(const QVector3D& position);

    Q_DECL_CONSTEXPR const QVector3D& position() const;
    void setPosition(const QVector3D& position);

    // OpenGL Helpers
    static const int PositionTupleSize = 3;
    static Q_DECL_CONSTEXPR int positionOffset();
    static Q_DECL_CONSTEXPR int stride();

  private:
    QVector3D m_position;
};

// Constructors
Q_DECL_CONSTEXPR inline Vertex::Vertex()
{
}
Q_DECL_CONSTEXPR inline Vertex::Vertex(const QVector3D& position): m_position(position)
{
}

// Accessors / Mutators
Q_DECL_CONSTEXPR inline const QVector3D& Vertex::position() const
{
    return m_position;
}
void inline Vertex::setPosition(const QVector3D& position)
{
    m_position = position;
}

// OpenGL Helpers
Q_DECL_CONSTEXPR inline int Vertex::positionOffset()
{
    return offsetof(Vertex, m_position);
}
Q_DECL_CONSTEXPR inline int Vertex::stride()
{
    return sizeof(Vertex);
}

// Note: Q_MOVABLE_TYPE means it can be memcpy'd.
Q_DECLARE_TYPEINFO(Vertex, Q_MOVABLE_TYPE);
