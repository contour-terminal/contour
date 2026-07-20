// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QAccessible>

#include <utility>

namespace contour::display
{

/// Speaks a short message to whatever assistive technology is listening.
///
/// Some things a terminal does have no representation in the accessibility tree at all: a bell rings,
/// a notification arrives, a mode flips. There is no object whose state changed, so nothing is
/// announced unless it is announced explicitly.
///
/// An interface rather than a free function, because the DECISIONS -- what is worth saying, and how
/// urgently -- are the part worth testing, and offscreen QPA has no accessibility bridge to observe the
/// delivery through. A recording implementation makes those decisions assertable headlessly.
class Announcer
{
  public:
    Announcer() = default;
    Announcer(Announcer const&) = delete;
    Announcer& operator=(Announcer const&) = delete;
    Announcer(Announcer&&) = delete;
    Announcer& operator=(Announcer&&) = delete;
    virtual ~Announcer() = default;

    /// Says @p message.
    ///
    /// @param message    What to say. Empty messages are dropped by the implementations.
    /// @param politeness Whether this may wait for the client to finish what it is saying (Polite), or
    ///                   should interrupt it (Assertive).
    virtual void announce(QString const& message, QAccessible::AnnouncementPoliteness politeness) = 0;

    /// Says @p message politely -- the right choice for anything the user did not just ask for.
    void announce(QString const& message) { announce(message, QAccessible::AnnouncementPoliteness::Polite); }
};

/// Announces through Qt, against an anchor object that is already in the accessibility tree.
///
/// ALWAYS posts, never announces inline. Two independent reasons, either of which alone is fatal:
/// QAccessible::updateAccessibility is main-thread-only, and the call sites (bell,
/// showDesktopNotification) run on the TERMINAL thread with a non-recursive state mutex held -- so
/// touching terminal state from the announcement would self-deadlock. Nothing in the posted lambda may
/// read the terminal; the message is built by the caller and captured by value.
class QtAnnouncer final: public Announcer
{
  public:
    /// @param anchor The object an assistive client already knows about. Must outlive this announcer.
    explicit QtAnnouncer(QObject* anchor): _anchor { anchor } {}

    void announce(QString const& message, QAccessible::AnnouncementPoliteness politeness) override
    {
        if (_anchor == nullptr || message.isEmpty())
            return;

        QMetaObject::invokeMethod(
            _anchor,
            [anchor = _anchor, message, politeness]() {
                auto event = QAccessibleAnnouncementEvent(anchor, message);
                event.setPoliteness(politeness);
                QAccessible::updateAccessibility(&event);
            },
            Qt::QueuedConnection);
    }

  private:
    QObject* _anchor;
};

/// An Announcer that says nothing.
///
/// The default wherever no anchor object exists yet (a display-less background session, a headless
/// test), so call sites never have to ask whether announcing is possible.
class NullAnnouncer final: public Announcer
{
  public:
    void announce(QString const& /*message*/, QAccessible::AnnouncementPoliteness /*politeness*/) override {}
};

} // namespace contour::display
