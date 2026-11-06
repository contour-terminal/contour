// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A readiness probe that runs nothing, for the tests of what upgrades a daemon in place.

#include <core/async/Task.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <vthost/handover/Readiness.hpp>

namespace vthost::testing
{

/// A ReadinessProbe that answers as told, and records whom it was asked about.
class FakeReadinessProbe final: public handover::ReadinessProbe
{
  public:
    /// @param release The release the target reports, which reads every format this build writes and adopts
    ///        pseudoconsoles.
    explicit FakeReadinessProbe(std::string release = "0.9.1"):
        answer { handover::ownReadiness(std::move(release), handover::PseudoconsoleAdoption::Able) }
    {
    }

    [[nodiscard]] core::async::Task<std::expected<handover::ReadinessReport, handover::UpgradeRefusal>> probe(
        handover::ReadinessQuery query) override
    {
        asked.push_back(query.executable);
        queries.push_back(std::move(query));
        co_return answer;
    }

    /// What every probe() answers.
    std::expected<handover::ReadinessReport, handover::UpgradeRefusal> answer;

    /// Every executable probe() was asked about, in order.
    std::vector<std::filesystem::path> asked;

    /// Every query probe() was asked, in order.
    std::vector<handover::ReadinessQuery> queries;
};

} // namespace vthost::testing
