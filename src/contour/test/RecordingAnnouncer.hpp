// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/Announcer.hpp>

#include <QtCore/QString>
#include <QtGui/QAccessible>

#include <vector>

namespace contour::test
{

/// An Announcer that records instead of speaking, so the DECISIONS are assertable with no
/// accessibility bridge in sight (offscreen QPA has none).
class RecordingAnnouncer final: public platform::Announcer
{
  public:
    /// One thing said, and how urgently.
    struct Said
    {
        QString message;
        QAccessible::AnnouncementPoliteness politeness;
    };

    void announce(QString const& message, QAccessible::AnnouncementPoliteness politeness) override
    {
        said.push_back({ message, politeness });
    }

    std::vector<Said> said; ///< Everything said, in order.
};

} // namespace contour::test
