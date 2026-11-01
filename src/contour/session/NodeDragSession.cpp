// SPDX-License-Identifier: Apache-2.0
#include <contour/session/NodeDragSession.hpp>
#include <contour/session/TerminalSession.hpp>

namespace contour::session
{

NodeDragSession::NodeDragSession(QObject const& owner,
                                 QString payload,
                                 ResolvedDrag drag,
                                 TerminalSession* carried,
                                 window::DragEndProbe& probe):
    _owner(&owner), _payload(std::move(payload)), _drag(std::move(drag)), _carried(carried), _probe(probe)
{
}

NodeDragSession::~NodeDragSession()
{
    _probe.get().forgetDrag();
}

TerminalSession* NodeDragSession::carried() const noexcept
{
    return static_cast<TerminalSession*>(_carried.data());
}

} // namespace contour::session
