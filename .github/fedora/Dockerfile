FROM fedora:35

WORKDIR /app

# Update packages and install RPM development tools
RUN dnf upgrade -y
RUN dnf install -y \
        gcc rpm-build rpm-devel make coreutils diffutils patch rpmdevtools

# Install contour dependencies
RUN dnf install -y \
        cmake extra-cmake-modules ninja-build gcc-c++ pkgconf freetype-devel harfbuzz-devel \
        qt5-qtbase-devel qt5-qtbase-gui kf5-kwindowsystem-devel

RUN useradd -d /app builder
RUN echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
USER builder

RUN sudo chown builder:builder .

# Setup RPM build directories
RUN mkdir -p /app/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Clone contour, copy tarball and spec file to rpmbuild directories
RUN git clone https://github.com/contour.git . \
    && cd contour \
    && export version="$(cat .github/fedora/contour.spec | awk -F: '$1=="Version" { print $2 }' | tr -d ' ')" \
    && git archive --output=/app/rpmbuild/SOURCES/contour-${version}.tar.gz --prefix=contour-${version}/ HEAD \
    && cp .github/fedora/contour.spec /app/rpmbuild/SPECS/contour.spec

# Build contour
RUN cd /app/rpmbuild \
    && rpmbuild --define "_topdir $(pwd)" -v -bb SPECS/contour.spec
