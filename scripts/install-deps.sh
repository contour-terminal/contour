#! /bin/sh

set -e

if test x$QTVER = x; then
    QTVER=6
fi

# Special environment variable to be used when only fetching and extracting
# embedded dependencies should be done, i.e. no system package manager is
# being invoked.
#
# set this as environment variable to ON to activate this mode.
if [ x$PREPARE_ONLY_EMBEDS = x ]
then
    PREPARE_ONLY_EMBEDS=OFF
fi

# if SYSDEP_ASSUME_YES=ON is set, then system package managers are attempted
# to install packages automatically, i.e. without confirmation.
if [ x$SYSDEP_ASSUME_YES = xON ]
then
    SYSDEP_ASSUME_YES='-y'
else
    unset SYSDEP_ASSUME_YES
fi

# {{{ sysdeps fetcher and unpacker for deps that aren't available via sys pkg mgnr
SYSDEPS_BASE_DIR="$(dirname $0)/../_deps"

SYSDEPS_DIST_DIR="$SYSDEPS_BASE_DIR/distfiles"
SYSDEPS_SRC_DIR="$SYSDEPS_BASE_DIR/sources"
SYSDEPS_CMAKE_FILE="$SYSDEPS_SRC_DIR/CMakeLists.txt"

fetch_and_unpack()
{
    NAME=$1
    DISTFILE=$2
    URL=$3
    MACRO=$4

    FULL_DISTFILE="$SYSDEPS_DIST_DIR/$DISTFILE"

    if ! test -f "$FULL_DISTFILE"; then
        if command -v wget > /tmp/word 2>&1; then
            wget -O "$FULL_DISTFILE" "$URL"
        elif command -v curl > /tmp/word 2>&1; then
            curl -L -o "$FULL_DISTFILE" "$URL"
        elif command -v fetch > /tmp/word 2>&1; then
            # FreeBSD
            fetch -o "$FULL_DISTFILE" "$URL"
        else
            echo "Don't know how to fetch from the internet." 1>&2
            exit 1
        fi
    else
        echo "Already fetched $DISTFILE. Skipping."
    fi

    if ! test -d "$SYSDEPS_SRC_DIR/$NAME"; then
        echo "Extracting $DISTFILE"
        tar xzpf $FULL_DISTFILE -C $SYSDEPS_SRC_DIR
    else
        echo "Already extracted $DISTFILE. Skipping."
    fi

    if test x$MACRO = x; then
        echo "add_subdirectory($NAME EXCLUDE_FROM_ALL)" >> $SYSDEPS_CMAKE_FILE
    else
        echo "macro(ContourThirdParties_Embed_$MACRO)" >> $SYSDEPS_CMAKE_FILE
        echo "    add_subdirectory(\${ContourThirdParties_SRCDIR}/$NAME EXCLUDE_FROM_ALL)" >> $SYSDEPS_CMAKE_FILE
        echo "endmacro()" >> $SYSDEPS_CMAKE_FILE
    fi
}

fetch_and_unpack_Catch2()
{
    fetch_and_unpack \
        Catch2-3.4.0 \
        Catch2-3.4.0.tar.gz \
        https://github.com/catchorg/Catch2/archive/refs/tags/v3.4.0.tar.gz
}

fetch_and_unpack_fmtlib()
{
    fmtlib_version="10.0.0"
    fetch_and_unpack \
        fmt-$fmtlib_version \
        fmtlib-$fmtlib_version.tar.gz \
        https://github.com/fmtlib/fmt/archive/refs/tags/$fmtlib_version.tar.gz
}

fetch_and_unpack_gsl()
{
    fetch_and_unpack \
        GSL-3.1.0 \
        gsl-3.1.0.tar.gz \
        https://github.com/microsoft/GSL/archive/refs/tags/v3.1.0.tar.gz
}

fetch_and_unpack_termbenchpro()
{
    local termbench_pro_git_sha="7f86c882b2dab88a0cceeffd7e3848f55fa5f6f2"
    fetch_and_unpack \
        termbench-pro-$termbench_pro_git_sha \
        termbench-pro-$termbench_pro_git_sha.tar.gz \
        https://github.com/contour-terminal/termbench-pro/archive/$termbench_pro_git_sha.tar.gz \
        termbench_pro
}

fetch_and_unpack_boxed()
{
    local boxed_cpp_version="1.1.0"
    fetch_and_unpack \
        boxed-cpp-$boxed_cpp_version \
        boxed-cpp-$boxed_cpp_version.tar.gz \
        https://github.com/contour-terminal/boxed-cpp/archive/refs/tags/v${boxed_cpp_version}.tar.gz \
        boxed_cpp
}

fetch_and_unpack_libunicode()
{
    if test x$LIBUNICODE_SRC_DIR = x; then
        local libunicode_git_sha="23d7b30166a914b10526bb8fe7a469a9610c07dc"
        fetch_and_unpack \
            libunicode-$libunicode_git_sha \
            libunicode-$libunicode_git_sha.tar.gz \
            https://github.com/contour-terminal/libunicode/archive/$libunicode_git_sha.tar.gz \
            libunicode
    else
        echo "Hard linking external libunicode source directory to: $LIBUNICODE_SRC_DIR"
        MACRO="libunicode"
        echo "macro(ContourThirdParties_Embed_$MACRO)" >> $SYSDEPS_CMAKE_FILE
        echo "    add_subdirectory($LIBUNICODE_SRC_DIR libunicode EXCLUDE_FROM_ALL)" >> $SYSDEPS_CMAKE_FILE
        echo "endmacro()" >> $SYSDEPS_CMAKE_FILE
    fi
}

fetch_and_unpack_yaml_cpp()
{
    fetch_and_unpack \
        yaml-cpp-yaml-cpp-0.7.0 \
        yaml-cpp-0.7.0.tar.gz \
        https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.7.0.tar.gz
}

fetch_and_unpack_range()
{
    fetch_and_unpack \
        range-v3-0.12.0 \
        range-v3-0.12.0.tar.gz \
        https://github.com/ericniebler/range-v3/archive/refs/tags/0.12.0.tar.gz
}

fetch_and_unpack_libutempter()
{
    local libutempter_version="1.2.1"
    fetch_and_unpack \
        libutempter-$libutempter_version \
        libutempter-$libutempter_version.tar.gz \
        http://ftp.altlinux.org/pub/people/ldv/utempter/libutempter-$libutempter_version.tar.gz
    printf \
        "cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(utempter VERSION "$libutempter_version" LANGUAGES C)

add_library(utempter STATIC
        utempter.c
        utempter.h
        iface.c
        diag.h
)

target_include_directories(utempter PUBLIC \${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(utempter PUBLIC
"LIBEXECDIR=\"/usr/local/lib\"")" \
    > "$SYSDEPS_BASE_DIR/sources/libutempter-$libutempter_version/CMakeLists.txt"
}

prepare_fetch_and_unpack()
{
    mkdir -p "${SYSDEPS_BASE_DIR}"
    mkdir -p "${SYSDEPS_DIST_DIR}"
    mkdir -p "${SYSDEPS_SRC_DIR}"

    # empty out sysdeps CMakeLists.txt
    rm -f $SYSDEPS_CMAKE_FILE
}
# }}}

install_deps_popos()
{
    local packages="
        build-essential
        cmake
        debhelper
        dpkg-dev
        extra-cmake-modules
        g++
        libc6-dev
        libfontconfig1-dev
        libfreetype6-dev
        libharfbuzz-dev
        libqt5gui5
        libqt5opengl5-dev
        libqt5x11extras5-dev
        libssh2-1-dev
        libutempter-dev
        libx11-xcb-dev
        libyaml-cpp-dev
        make
        ninja-build
        ncurses-bin
        pkg-config
        qtbase5-dev
        qtmultimedia5-dev
    "

    RELEASE=`grep VERSION_ID /etc/os-release | cut -d= -f2 | tr -d '"'`

    local NAME=`grep ^NAME /etc/os-release | cut -d= -f2 | cut -f1 | tr -d '"'`

    fetch_and_unpack_libunicode
    fetch_and_unpack_gsl
    fetch_and_unpack_fmtlib
    fetch_and_unpack_range
    fetch_and_unpack_Catch2

    [ x$PREPARE_ONLY_EMBEDS = xON ] && return

    sudo apt install $SYSDEP_ASSUME_YES $packages
    # sudo snap install --classic powershell
}

install_deps_ubuntu()
{
    fetch_and_unpack_libunicode
    local packages="
        build-essential
        cmake
        debhelper
        dpkg-dev
        extra-cmake-modules
        g++
        libc6-dev
        libfontconfig1-dev
        libfreetype6-dev
        libharfbuzz-dev
        libssh2-1-dev
        libutempter-dev
        libx11-xcb-dev
        libyaml-cpp-dev
        make
        ninja-build
        ncurses-bin
        pkg-config
    "

    if test x$QTVER = x6; then
        packages="$packages
            libgl1-mesa-dev
            libglvnd-dev
            libqt6core5compat6-dev
            libqt6opengl6-dev
            qml6-module-qt-labs-platform
            qml6-module-qtqml-workerscript
            qml6-module-qtquick-controls
            qml6-module-qtquick-layouts
            qml6-module-qtmultimedia
            qml6-module-qtquick-templates
            qml6-module-qtquick-window
            qml6-module-qt5compat-graphicaleffects
            qt6-base-dev
            qt6-base-dev-tools
            qt6-declarative-dev
            qt6-multimedia-dev
            qt6-qpa-plugins
        "
    else
        packages="$packages
            libqt5gui5
            libqt5opengl5-dev
            libqt5x11extras5-dev
            qtbase5-dev
            qtdeclarative5-dev
            qtmultimedia5-dev
            qtquickcontrols2-5-dev
            qml-module-qtmultimedia
            qml-module-qtquick-controls
            qml-module-qtquick-controls2
            qml-module-qt-labs-platform
        "
    fi

    RELEASE=`grep VERSION_ID /etc/os-release | cut -d= -f2 | tr -d '"'`

    local NAME=`grep ^NAME /etc/os-release | cut -d= -f2 | cut -f1 | tr -d '"'`

    if [ ! "${NAME}" = "Debian GNU/Linux" ]; then
        # We cannot use [[ nor < here because that's not in /bin/sh.
        if [ "$RELEASE" = "18.04" ]; then
            # Old Ubuntu's (especially 18.04 LTS) doesn't have a proper std::filesystem implementation.
            packages="$packages g++-8"
        fi
        if [ "$RELEASE" = "22.04" ] || [ "$RELEASE" = "22.10"  ]; then
            packages="$packages"
        fi
        if [ "$RELEASE" = "23.04" ]; then
            packages="$packages qml6-moduile-qtquick3d-spatialaudio"
        fi

    fi

    fetch_and_unpack_gsl
    case $RELEASE in
        "18.04" | "19.04" | "20.04" | "21.04" | "21.10" | "22.04" | "23.04")
            # Older Ubuntu's don't have a recent enough fmt / range-v3, so supply it.
            fetch_and_unpack_range
            fetch_and_unpack_fmtlib
            fetch_and_unpack_Catch2
            ;;
        *)
            packages="$packages libfmt-dev librange-v3-dev catch2"
            ;;
    esac

    [ x$PREPARE_ONLY_EMBEDS = xON ] && return

    sudo apt install $SYSDEP_ASSUME_YES $packages
    # sudo snap install --classic powershell
}

install_deps_FreeBSD()
{
    fetch_and_unpack_fmtlib
    fetch_and_unpack_libunicode
    fetch_and_unpack_Catch2

    [ x$PREPARE_ONLY_EMBEDS = xON ] && return

    # NB: libfmt is available in pkg, but it's not version >= 9.0.0 (as of 2022-09-03).
    # NB: catch2 (as name "catch") is available in pkg, but it's not version >= 3.0.0.
    su root -c "pkg install $SYSDEP_ASSUME_YES \
        cmake \
        fontconfig \
        freetype2 \
        harfbuzz \
        libssh2 \
        microsoft-gsl \
        ncurses \
        ninja \
        pkgconf \
        qt6-5compat \
        qt6-base \
        qt6-declarative \
        qt6-multimedia \
        qt6-tools \
        range-v3 \
        xcb \
        yaml-cpp
    "
}

install_deps_arch()
{
    fetch_and_unpack_libunicode
    fetch_and_unpack_fmtlib
    [ x$PREPARE_ONLY_EMBEDS = xON ] && return

    packages="
        catch2 \
        cmake \
        extra-cmake-modules \
        fontconfig \
        git \
        harfbuzz \
        libssh2 \
        libxcb \
        microsoft-gsl \
        ninja \
        pkg-config \
        range-v3 \
        libutempter \
        yaml-cpp \
    "

    if test x$QTVER = x6; then
        packages="$packages \
            qt6-5compat \
            qt6-base \
            qt6-declarative \
            qt6-multimedia \
            qt6-shadertools \
            qt6-wayland \
        "
    else
        packages="$packages \
            qt5-base \
            qt5-multimedia \
            qt5-x11extras \
        "
    fi

    sudo pacman -S -y $packages
}

install_deps_suse()
{
    fetch_and_unpack_libunicode
    fetch_and_unpack_gsl
    fetch_and_unpack_fmtlib

    echo "SuSE: PREPARE_ONLY_EMBEDS=$PREPARE_ONLY_EMBEDS"
    [ x$PREPARE_ONLY_EMBEDS = xON ] && return

    local packages="
        Catch2-devel
        cmake
        extra-cmake-modules
        fontconfig-devel
        freetype-devel
        gcc-c++
        harfbuzz-devel
        utempter-devel
        libxcb-devel
        libssh2-devel
        ncurses-devel
        ninja
        pkgconf
        range-v3-devel
        yaml-cpp-devel
    "

    if test x$QTVER = x6; then
        packages="$packages
            qt6-base-common-devel
            qt6-base-devel
            qt6-gui-devel
            qt6-multimedia-devel
            qt6-opengl-devel
            qt6-qml-devel
            qt6-qt5compat-devel
            qt6-quick-devel
            qt6-quickcontrols2-devel
        "
    else
        packages="$packages
            libqt5-qtbase
            libqt5-qtbase-common-devel
            libqt5-qtbase-devel
            libqt5-qtmultimedia-devel
            libqt5-qtx11extras-devel

        "
    fi
    # Sadly, gsl-devel system package is too old to be used.
    sudo zypper install $SYSDEP_ASSUME_YES $packages
}

install_deps_fedora()
{
    local os_version=`grep VERSION_ID /etc/os-release | cut -d= -f2 | tr -d '"'`

    local packages="
        catch-devel
        cmake
        extra-cmake-modules
        fontconfig-devel
        freetype-devel
        gcc-c++
        harfbuzz-devel
        libssh2-devel
        libxcb-devel
        ninja-build
        pkgconf
        range-v3-devel
        libutempter-devel
        yaml-cpp-devel
    "

    fetch_and_unpack_libunicode
    fetch_and_unpack_gsl

    if test "$os_version" -ge 39; then
        packages="$packages fmt-devel"
    else
        fetch_and_unpack_fmtlib
    fi

    [ x$PREPARE_ONLY_EMBEDS = xON ] && return

    if test x$QTVER = x6; then
        packages="$packages
            qt6-qt5compat-devel
            qt6-qtbase-devel
            qt6-qtbase-gui
            qt6-qtdeclarative-devel
            qt6-qtmultimedia-devel
            qt6-qtwayland
            qt6-qtquickcontrols2-devel
        "
    else
        packages="$packages
            qt5-qtbase-devel
            qt5-qtbase-gui
            qt5-qtmultimedia-devel
            qt5-qtx11extras-devel
            qt5-qtquickcontrols2-devel
        "
    fi
    # Sadly, gsl-devel system package is too old to be used.
    sudo dnf install $SYSDEP_ASSUME_YES $packages
}


install_deps_darwin()
{
    fetch_and_unpack_libunicode

    [ x$PREPARE_ONLY_EMBEDS = xON ] && return

    # NB: Also available in brew: mimalloc
    HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 brew install $SYSDEP_ASSUME_YES \
        catch2 \
        cpp-gsl \
        fmt \
        fontconfig \
        freetype \
        harfbuzz \
        pkg-config \
        qt$QTVER \
        libssh2 \
        range-v3 \
        yaml-cpp
}

main()
{
    if test x$OS_OVERRIDE != x; then
        # In CI, we need to be able to fetch embedd-setups for different OSes.
        ID=$OS_OVERRIDE
    elif test -f /etc/os-release; then
        ID=`grep ^ID= /etc/os-release | cut -d= -f2`
    else
        ID=`uname -s`
    fi

    # Strip double-quotes, as used by opensuse for interesting reason.
    ID=`echo $ID | tr -d '"'`

    prepare_fetch_and_unpack

    case "$ID" in
        arch|manjaro)
            install_deps_arch
            ;;
        opensuse*)
            install_deps_suse
            ;;
        fedora)
            install_deps_fedora
            ;;
        pop)
            install_deps_popos
            ;;
        ubuntu|neon)
            install_deps_ubuntu
            ;;
        debian)
            install_deps_ubuntu
            fetch_and_unpack_fmtlib
            ;;
        Darwin)
            install_deps_darwin
            ;;
        FreeBSD)
            install_deps_FreeBSD
            ;;
        *)
            fetch_and_unpack_Catch2
            fetch_and_unpack_fmtlib
            fetch_and_unpack_gsl
            fetch_and_unpack_yaml_cpp
            fetch_and_unpack_range
            fetch_and_unpack_libunicode
            fetch_and_unpack_libutempter
            echo "OS $ID not supported."
            echo "Please install the remaining dependencies manually."
            echo "Most importantly: Qt, freetype, harfbuzz (including development headers)."
            ;;
    esac

    fetch_and_unpack_boxed
    fetch_and_unpack_termbenchpro
}

main $*
