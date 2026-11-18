// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <vtextension/CommandPolicy.hpp>

namespace vtextension::testing
{

/// An in-memory grant store: an answer is what record() last recorded for the same identity and digest,
/// `Pending` before that.
class FakeGrantStore final: public IGrantStore
{
  public:
    [[nodiscard]] GrantState lookup(std::string_view hostId, std::string_view digest) const override
    {
        auto const it = _grants.find({ std::string(hostId), std::string(digest) });
        return it != _grants.end() ? it->second : GrantState::Pending;
    }

    void record(std::string_view hostId, std::string_view digest, GrantState state) override
    {
        _grants[{ std::string(hostId), std::string(digest) }] = state;
    }

  private:
    std::map<std::pair<std::string, std::string>, GrantState> _grants;
};

} // namespace vtextension::testing
