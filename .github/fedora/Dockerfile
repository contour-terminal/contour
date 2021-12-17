FROM fedora:latest AS base

ARG VERSION
ARG CONTOUR_VERSION=$VERSION

WORKDIR /app
COPY . /app

# Install Git and RPM development tools
RUN dnf install -y \
        git gcc rpm-build rpm-devel make coreutils diffutils patch rpmdevtools desktop-file-utils

# Install contour dependencies
RUN SYSDEP_ASSUME_YES=ON ./scripts/install-deps.sh

RUN useradd -d /app builder
RUN echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
USER builder

RUN sudo chown builder:builder .

# Setup RPM build directories
RUN mkdir -p /app/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

RUN cp .github/fedora/contour.spec /app/rpmbuild/SPECS/contour.spec
RUN mv contour-${CONTOUR_VERSION}.tar.gz /app/rpmbuild/SOURCES/contour-${CONTOUR_VERSION}.tar.gz

# Build contour
RUN cd /app/rpmbuild \
    && rpmbuild --define "_topdir $(pwd)" -v -ba SPECS/contour.spec
