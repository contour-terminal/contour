// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QCoreApplication>

namespace contour::test
{

/// Turns a Qt application attribute on for its lifetime, and restores the state it found on destruction.
///
/// For a test that must see what the app sets before its QApplication exists — Qt::AA_MacDontSwapCtrlAndMeta,
/// say — without leaking it into the cases that run after it.
class ScopedApplicationAttribute
{
  public:
    /// @param attribute The attribute to turn on.
    explicit ScopedApplicationAttribute(Qt::ApplicationAttribute attribute):
        _attribute(attribute), _wasSetBefore(QCoreApplication::testAttribute(attribute))
    {
        QCoreApplication::setAttribute(_attribute, true);
    }

    ~ScopedApplicationAttribute() { QCoreApplication::setAttribute(_attribute, _wasSetBefore); }

    ScopedApplicationAttribute(ScopedApplicationAttribute const&) = delete;
    ScopedApplicationAttribute(ScopedApplicationAttribute&&) = delete;
    ScopedApplicationAttribute& operator=(ScopedApplicationAttribute const&) = delete;
    ScopedApplicationAttribute& operator=(ScopedApplicationAttribute&&) = delete;

  private:
    Qt::ApplicationAttribute _attribute; ///< The attribute held on.
    /// Its state before, restored on destruction. A plain bool by AGENT.md's "signature you do not own"
    /// carve-out: it mirrors the bool Qt's testAttribute()/setAttribute() speak.
    bool _wasSetBefore;
};

} // namespace contour::test
