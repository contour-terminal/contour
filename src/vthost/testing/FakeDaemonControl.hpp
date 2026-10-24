// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A DaemonControl that serves every request and records what it was asked: a test's daemon.

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/ClientCensus.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::testing
{

/// A daemon of release "9.9.9" with no sessions, which counts its clients in @ref census and records every
/// stop it is asked for.
class FakeDaemonControl final: public DaemonControl, public CensusEvents
{
  public:
    /// The release it states, in its ServerHello and its status alike.
    static constexpr std::string_view Release = "9.9.9";

    FakeDaemonControl() { census.subscribe(this); }
    FakeDaemonControl(FakeDaemonControl const&) = delete;
    FakeDaemonControl& operator=(FakeDaemonControl const&) = delete;
    FakeDaemonControl(FakeDaemonControl&&) = delete;
    FakeDaemonControl& operator=(FakeDaemonControl&&) = delete;
    ~FakeDaemonControl() override { census.unsubscribe(this); }

    [[nodiscard]] std::expected<proto::DaemonStatusReply, DaemonControlError> status() const override
    {
        return proto::DaemonStatusReply { .releaseVersion = std::string { Release }, .sessions = 0 };
    }

    [[nodiscard]] std::string_view releaseVersion() const noexcept override { return Release; }

    [[nodiscard]] std::expected<void, DaemonControlError> requestStop(proto::StopReason reason) override
    {
        stops.push_back(reason);
        return {};
    }

    [[nodiscard]] ClientCensus::Ticket admitClient() override { return census.admit(); }

    void clientCountChanged(std::size_t count) override { peakClients = std::max(peakClients, count); }

    ClientCensus census;                  ///< Counts the clients it admitted.
    std::vector<proto::StopReason> stops; ///< Every stop asked for, in order.
    std::size_t peakClients = 0;          ///< The most clients @ref census counted at once.
};

} // namespace vthost::testing
