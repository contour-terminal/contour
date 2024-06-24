#!/usr/bin/env sh
set -ex

fetch(){
    url=$1
    wget $url
    tar xf $(basename $url)
    rm $(basename $url)
}

configure_install() {
    saved_dir=$(pwd)
    cd $1
    shift 1
    ./configure $@
    make -j
    make install
    cd $saved_dir
}

autogen_install() {
    saved_dir=$(pwd)
    cd $1
    shift 1
    if [ -f autogen.sh ]; then
        ./autogen.sh
    fi
    ./configure $@
    make -j
    make install
    cd $saved_dir
}


cmake_install(){
    saved_dir=$(pwd)
    cd $1
    shift 1
    cmake -S . -B build -G Ninja -D BUILD_SHARED_LIBS=OFF $@
    cmake --build build
    cmake --build build --target install
    cd $saved_dir
}

meson_install(){
    saved_dir=$(pwd)
    cd $1
    shift 1
    meson setup build --default-library static --buildtype release $@
    ninja -C build
    ninja -C build install
    cd $saved_dir
}

func_install_package(){
    func_install=$1
    url=$2
    fetch $url
    shift 2
    $func_install $(basename $url .tar.gz) $@
}

configure_install_package() {
    url=$1
    shift 1
    func_install_package configure_install $url $@ --enable-static --disable-shared
}

autogen_install_package() {
    url=$1
    shift 1
    func_install_package autogen_install $url $@ --enable-static --disable-shared
}

cmake_install_package() {
    dir=$1
    url=$2
    fetch $url
    shift 2
    cmake_install $dir $@
}

meson_install_package() {
    url=$1
    fetch $url
    shift 1
    meson_install $(basename $url .tar.xz) $@
}


# install libpng
configure_install_package "http://ftp-osl.osuosl.org/pub/libpng/src/libpng16/libpng-1.6.34.tar.gz" --enable-maintainer-mode
# install libb2
autogen_install_package "https://github.com/BLAKE2/libb2/releases/download/v0.98.1/libb2-0.98.1.tar.gz" --disable-dependency-tracking
# install pcre
autogen_install_package "https://sourceforge.net/projects/pcre/files/pcre/8.45/pcre-8.45.tar.gz"
# install pcre2
autogen_install_package "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz" --enable-pcre2-16
# install double-conversion
cmake_install_package "double-conversion-3.3.0" "https://github.com/google/double-conversion/archive/refs/tags/v3.3.0.tar.gz"
# install libX11
configure_install_package "https://www.x.org/releases/individual/lib/libX11-1.8.9.tar.gz"
