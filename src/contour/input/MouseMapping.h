// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>
#include <vtbackend/Terminal.h> // vtbackend::ScrollPhase

#include <QtCore/Qt>

#include <cstdint>

namespace contour::input
{

/// The mouse cursor shape the terminal asks its display to show.
///
/// Named after the intent rather than after Qt's enumerators, so that the terminal layer can decide
/// a shape without depending on the toolkit that draws it; toQtMouseShape() is the only translation.
enum class MouseCursorShape : uint8_t
{
    Hidden,
    PointingHand,
    IBeam,
    Arrow,
};

/// @param shape The shape the terminal asked for.
/// @return The Qt cursor shape that renders it.
[[nodiscard]] constexpr Qt::CursorShape toQtMouseShape(MouseCursorShape shape)
{
    switch (shape)
    {
        case MouseCursorShape::Hidden: return Qt::CursorShape::BlankCursor;
        case MouseCursorShape::Arrow: return Qt::CursorShape::ArrowCursor;
        case MouseCursorShape::IBeam: return Qt::CursorShape::IBeamCursor;
        case MouseCursorShape::PointingHand: return Qt::CursorShape::PointingHandCursor;
    }

    // should never be reached
    return Qt::CursorShape::ArrowCursor;
}

/// @param button The Qt mouse button.
/// @return The VT mouse button it reports as; Left for anything unrecognized.
[[nodiscard]] constexpr vtbackend::MouseButton makeMouseButton(Qt::MouseButton button)
{
    switch (button)
    {
        case Qt::MouseButton::RightButton: return vtbackend::MouseButton::Right;
        case Qt::MiddleButton: return vtbackend::MouseButton::Middle;
        case Qt::LeftButton: [[fallthrough]];
        default: // d'oh
            return vtbackend::MouseButton::Left;
    }
}

/// Maps Qt's scroll phase onto the platform-independent vtbackend enum.
///
/// Shared rather than local to the QWheelEvent path, because the QML wheel handler needs the same
/// mapping: QML hands a phase across as a plain int, and the two enumerations agreeing numerically
/// today is a coincidence to translate through, not one to rely on.
///
/// @param phase The phase Qt reported.
/// @return The corresponding vtbackend phase; NoPhase for anything unrecognized.
[[nodiscard]] vtbackend::ScrollPhase mapScrollPhase(Qt::ScrollPhase phase) noexcept;

} // namespace contour::input
