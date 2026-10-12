// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Config.readBufferSize.defaultsTo64KiB", "[config]")
{
    // A read takes at most this many bytes from the PTY, and a batch is parsed in one hold of the
    // terminal's lock: 64 KiB is the batch Alacritty holds its lock for.
    CHECK(contour::config::Config {}.ptyReadBufferSize.value() == 65536);
}

TEST_CASE("Config.readBufferSize.documentationNamesTheDefault", "[config]")
{
    // The option's web documentation takes the default from the configuration instead of repeating it.
    CHECK(contour::config::documentationGlobalConfig().contains("The default value is `65536`."));
}
