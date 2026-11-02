# The Fedora to build for: a release tag (the latest stable is the default) or `rawhide`.
ARG FEDORA_VERSION=44
FROM fedora:${FEDORA_VERSION} AS base

ARG VERSION
ARG CONTOUR_VERSION=$VERSION
# The spec reads the version from the environment (%{getenv:CONTOUR_VERSION}).
ENV CONTOUR_VERSION=${CONTOUR_VERSION}

WORKDIR /app
COPY . /app

# Install Git and RPM development tools (sudo: install-deps.sh uses it, and the minimal image has none)
RUN dnf install -y \
        git gcc rpm-build rpm-devel make coreutils diffutils patch rpmdevtools desktop-file-utils sudo

# Install contour dependencies
RUN SYSDEP_ASSUME_YES=ON QTVER=6 ./scripts/install-deps.sh

RUN useradd -d /app builder
RUN echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
USER builder

RUN sudo chown builder:builder .

# Setup RPM build directories
RUN mkdir -p /app/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

RUN cp .github/fedora/contour.spec /app/rpmbuild/SPECS/contour.spec
RUN mv contour-${CONTOUR_VERSION}.tar.gz /app/rpmbuild/SOURCES/contour-${CONTOUR_VERSION}.tar.gz

# Build both packages from the one spec: contour (the GUI terminal), then contour-daemon (--with daemon,
# the Qt-free GUI-less build). Both land in /app/rpmbuild/RPMS/<arch>/.
RUN cd /app/rpmbuild \
    && rpmbuild --define "_topdir $(pwd)" -v -ba SPECS/contour.spec \
    && rpmbuild --define "_topdir $(pwd)" -v -ba --with daemon SPECS/contour.spec
