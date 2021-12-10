FROM fedora:latest AS base

ARG VERSION_STRING
ARG CONTOUR_VERSION=$VERSION_STRING

WORKDIR /app

# Install Git and RPM development tools
RUN dnf install -y \
        git gcc rpm-build rpm-devel make coreutils diffutils patch rpmdevtools desktop-file-utils

# Install contour dependencies
RUN dnf install -y \
        cmake extra-cmake-modules ninja-build gcc-c++ pkgconf freetype-devel harfbuzz-devel \
        qt5-qtbase-devel qt5-qtbase-gui kf5-kwindowsystem-devel fontconfig-devel

RUN useradd -d /app builder
RUN echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
USER builder

RUN sudo chown builder:builder .

# Setup RPM build directories
RUN mkdir -p /app/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Clone contour, copy tarball and spec file to rpmbuild directories
RUN git clone https://github.com/NTBBloodbath/contour.git \
    && cd contour \
    && cp .github/fedora/contour.spec /app/rpmbuild/SPECS/contour.spec \
    && git archive --output=/app/rpmbuild/SOURCES/contour-${CONTOUR_VERSION}.tar.gz --prefix=contour-${CONTOUR_VERSION}/ HEAD

# Build contour
RUN cd /app/rpmbuild \
    && rpmbuild --define "_topdir $(pwd)" -v -ba SPECS/contour.spec
