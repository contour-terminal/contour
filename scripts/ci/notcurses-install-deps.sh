#! /bin/bash
set -ex

# Installs ONLY the test-harness runtime dependencies for the notcurses-demo / quick-shell-exit CI
# jobs: the virtual X server, the ffmpeg/notcurses media stack the demo renders, and the font stack.
#
# Contour's OWN runtime dependencies (Qt6, libyaml-cpp, libssh, …) are NOT listed here: they are
# declared by the .deb (CPACK_DEBIAN_PACKAGE_DEPENDS) and pulled automatically when the package is
# installed with `apt-get install ./<pkg>.deb`. Hand-maintaining exact Qt6 package names per Ubuntu
# release was a recurring breakage (the t64 transition, the libqt6multimediaquick6 rename on 26.04),
# so the package-manager resolves them from the .deb instead — correct for whatever distro CI runs on.

RELEASE=$(grep VERSION_ID /etc/os-release | cut -d= -f2 | tr -d '"')

# The notcurses demo's media codecs: development metapackages on newer releases, versioned runtime
# libs on 20.04 (where the -dev metapackages pulled the wrong ABI).
if [ "$RELEASE" = "20.04" ]; then
    media_packages="
        libavcodec58 libavdevice58 libavformat58 libavutil56
        libdeflate0 libncurses6 libqrcodegen1 libswscale5 libunistring2
    "
else
    media_packages="
        libavcodec-dev libavdevice-dev libavformat-dev libavutil-dev
        libdeflate-dev libncurses-dev libqrcodegen-dev libswscale-dev libunistring-dev
    "
fi

sudo apt install -y \
    xvfb \
    ffmpeg \
    $media_packages \
    libfontconfig1 \
    libfreetype6 \
    libharfbuzz0b \
    ncurses-bin \
    ${@}
