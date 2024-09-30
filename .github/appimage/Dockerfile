FROM ubuntu:24.04

RUN apt-get update
RUN apt-get install -y sudo clang wget

WORKDIR /
COPY . /contour
WORKDIR /contour

RUN SYSDEP_ASSUME_YES=ON sh ./scripts/install-deps.sh

RUN cmake -S . -B build -G Ninja  \
    -D CMAKE_BUILD_TYPE=RelWithDebInfo \
    -D CMAKE_CXX_COMPILER=clang++ \
    -D CONTOUR_USE_CPM=ON \
    -D CMAKE_INSTALL_PREFIX=/contour/AppDir

RUN cmake --build build
RUN cmake --build build --target install

RUN sudo apt install -y binutils coreutils desktop-file-utils fakeroot fuse libgdk-pixbuf2.0-dev patchelf python3-pip python3-setuptools squashfs-tools strace util-linux zsync
RUN sudo wget https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage -O /usr/local/bin/appimagetool
RUN sudo chmod +x /usr/local/bin/appimagetool
RUN sudo sudo pip3 install appimage-builder --break-system-packages

RUN appimage-builder --recipe $PWD/.github/appimage/AppImageBuilder.yml
