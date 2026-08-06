// SPDX-License-Identifier: Apache-2.0
#include <contour/input/MouseMapping.h>

namespace contour::input
{

vtbackend::ScrollPhase mapScrollPhase(Qt::ScrollPhase phase) noexcept
{
    switch (phase)
    {
        case Qt::ScrollBegin: return vtbackend::ScrollPhase::Begin;
        case Qt::ScrollUpdate: return vtbackend::ScrollPhase::Update;
        case Qt::ScrollEnd: return vtbackend::ScrollPhase::End;
        case Qt::ScrollMomentum: return vtbackend::ScrollPhase::Momentum;
        case Qt::NoScrollPhase: return vtbackend::ScrollPhase::NoPhase;
    }
    return vtbackend::ScrollPhase::NoPhase;
}

} // namespace contour::input
