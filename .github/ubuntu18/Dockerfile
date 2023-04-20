FROM ubuntu:18.04

# Install updates to base image
RUN \
    apt-get update \
    && apt-get install -y \
    git \
    wget \
    sudo \
    ninja-build \
    software-properties-common

RUN add-apt-repository ppa:ubuntu-toolchain-r/test
RUN apt-get update
RUN apt-get install -y g++-10

RUN apt-get purge -y --auto-remove cmake
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
RUN apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main'
RUN apt-get install -y cmake

WORKDIR /home/docker

RUN git clone --depth=1  https://github.com/contour-terminal/contour.git
WORKDIR /home/docker/contour
RUN SYSDEP_ASSUME_YES=ON ./scripts/install-deps.sh
RUN cmake --version
RUN g++-10 --version
RUN cmake -S . -B build -G Ninja \
    -D CMAKE_CXX_STANDARD=20 \
    -D CMAKE_CXX_COMPILER=g++-10

RUN cmake --build build -- -j3
RUN ./build/src/crispy/crispy_test
RUN ./build/src/vtparser/vtparser_test
RUN ./build/src/vtbackend/vtbackend_test
RUN cd build && cpack -G DEB -V .
RUN mv build/*.deb package.deb
RUN mv build/*.ddeb package.ddeb
