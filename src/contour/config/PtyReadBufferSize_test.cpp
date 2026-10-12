// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>

#include <vtbackend/screen/Settings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

TEST_CASE("Config.readBufferSize.defaultsTo64KiB", "[config]")
{
    // A read takes at most this many bytes from the PTY, and a batch is parsed in one hold of the
    // terminal's lock: 64 KiB is the batch Alacritty holds its lock for.
    CHECK(contour::config::Config {}.ptyReadBufferSize.value() == 65536);
}

TEST_CASE("Config.readBufferSize.agreesWithTheBackendDefault", "[config]")
{
    // A host that builds its terminal settings without a configuration -- the conformance harness, a
    // bare vthost::DaemonConfig -- reads in the same batches as one that has it.
    CHECK(std::cmp_equal(contour::config::Config {}.ptyReadBufferSize.value(),
                         vtbackend::Settings {}.ptyReadBufferSize));
}
