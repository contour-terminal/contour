// SPDX-License-Identifier: Apache-2.0
#pragma once

template <class... Ts>
struct overloaded: Ts... // NOLINT(readability-identifier-naming)
{
    using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;
