#! /bin/bash

set -e

install_deps_ubuntu()
{
    local RELEASE=`lsb_release -r | awk '{print $NF}'`

    local packages=(
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
        libkf5windowsystem-dev
        libqt5gui5
        libqt5opengl5-dev
        make
        ncurses-bin
        pkg-config
        qtbase5-dev
    )

    if [[ "${RELEASE}" < "19.04" ]]; then
        # Old Ubuntu's (especially 18.04 LTS) doesn't have a proper std::filesystem implementation.
        #packages+=libboost-all-dev
        packages=( ${packages[*]} libboost-filesystem-dev g++-8 )
    fi

    apt install ${packages[*]}
    sudo snap install --classic powershell
}

main_linux()
{
    local ID=`lsb_release --id | awk '{print $NF}'`

    case "${ID}" in
        Ubuntu)
            install_deps_ubuntu
            ;;
        *)
            echo "No automated installation of build dependencies available yet."
            ;;
    esac
}

main_darwin()
{
    brew install freetype fontconfig harfbuzz boost pkg-config
}

main()
{
    case "$OSTYPE" in
        linux-gnu*)
            main_linux
            ;;
        darwin*)
            main_darwin
            ;;
        *)
            echo "OS not supported."
            ;;
    esac
}

main
