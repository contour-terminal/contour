#! /bin/bash

set -ex

# Installs the C++ runtime the ubuntu-24.04 matrix's GCC leg builds against, for the jobs that run the test
# binaries that leg uploads. Ubuntu 24.04's own libstdc++ is GCC 14's; a binary built with a newer GCC needs
# that GCC's libstdc++ (GLIBCXX_3.4.34 and later), which comes from the same toolchain PPA the leg installs
# its compiler from.

sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update -q
sudo apt install -y libstdc++6
