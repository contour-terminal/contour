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
    if [ -f "autogen.sh" ]; then
        sh autogen.sh
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

configure_install_package() {
    url=$1
    shift 1
    fetch $url
    configure_install $(basename $url .tar.gz) $@
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

# install libb2
configure_install_package "https://github.com/BLAKE2/libb2/releases/download/v0.98.1/libb2-0.98.1.tar.gz" --disable-dependency-tracking --enable-static --disable-shared
# install pcre2
configure_install_package "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz" --enable-pcre2-16  --enable-static --disable-shared
# install double-conversion
cmake_install_package "double-conversion-3.3.0" "https://github.com/google/double-conversion/archive/refs/tags/v3.3.0.tar.gz"
# install libgraphite2
cmake_install_package "graphite2-1.3.14" "https://github.com/silnrsi/graphite/releases/download/1.3.14/graphite2-1.3.14.tgz"
# install harfbuzz
cmake_install_package "harfbuzz-8.5.0" "https://github.com/harfbuzz/harfbuzz/releases/download/8.5.0/harfbuzz-8.5.0.tar.xz"
