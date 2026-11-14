// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The location chip's fit steps as QML reads them: the one place that turns window::LocationChipSteps into
/// a QVariantList, shared by WindowController and the test doubles standing in for it.

#include <contour/window/LocationChip.hpp>

#include <QtCore/QString>
#include <QtCore/QVariant>

#include <utility>

namespace contour::window
{

/// @return Each of LocationChipSteps as a map { fit, key }, in elision order: a LocationChipFit's underlying
///         value and the name WindowController::chipFit() takes that step's width by.
[[nodiscard]] inline QVariantList locationChipStepList()
{
    auto steps = QVariantList {};
    for (auto const& step: LocationChipSteps)
        steps.push_back(
            QVariantMap { { QStringLiteral("fit"), static_cast<int>(std::to_underlying(step.fit)) },
                          { QStringLiteral("key"),
                            QString::fromUtf8(step.key.data(), static_cast<qsizetype>(step.key.size())) } });
    return steps;
}

} // namespace contour::window
