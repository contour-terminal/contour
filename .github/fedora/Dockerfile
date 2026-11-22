# The Fedora to build for: a release tag (the latest stable is the default) or `rawhide`.
ARG FEDORA_VERSION=44
FROM fedora:${FEDORA_VERSION} AS base

ARG VERSION
ARG CONTOUR_VERSION=$VERSION
# The wasi-sdk the agent orchestrator's guest is built with, and the SHA-256 of its x86_64 Linux tarball:
# build.yml's WASI_SDK_VERSION and WASI_SDK_X86_64_LINUX_SHA256, passed by the package_for_Fedora job.
ARG WASI_SDK_VERSION
ARG WASI_SDK_X86_64_LINUX_SHA256
# The spec reads the version from the environment (%{getenv:CONTOUR_VERSION}).
ENV CONTOUR_VERSION=${CONTOUR_VERSION}

WORKDIR /app
COPY . /app

# Install Git and RPM development tools (sudo: install-deps.sh uses it, and the minimal image has none)
RUN dnf install -y \
        git gcc rpm-build rpm-devel make coreutils diffutils patch rpmdevtools desktop-file-utils sudo \
        curl tar gzip

# wasi-sdk in /opt/wasi-sdk, checked against the release asset's digest before it is unpacked
# (`--with orchestrator` below).
RUN test -n "$WASI_SDK_VERSION" && test -n "$WASI_SDK_X86_64_LINUX_SHA256" \
    && name="wasi-sdk-${WASI_SDK_VERSION}.0-x86_64-linux" \
    && curl -fsSL --retry 5 --retry-all-errors --max-time 600 -o "/tmp/$name.tar.gz" \
        "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VERSION}/$name.tar.gz" \
    && echo "$WASI_SDK_X86_64_LINUX_SHA256  /tmp/$name.tar.gz" | sha256sum --check --strict \
    && mkdir -p /opt/wasi-sdk \
    && tar -xzf "/tmp/$name.tar.gz" -C /opt/wasi-sdk --strip-components=1 \
    && rm "/tmp/$name.tar.gz" \
    && test -x /opt/wasi-sdk/bin/clang++

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
# the Qt-free GUI-less build). Both land in /app/rpmbuild/RPMS/<arch>/. Both carry the agent orchestrator
# (--with orchestrator), built with the wasi-sdk above.
RUN cd /app/rpmbuild \
    && rpmbuild --define "_topdir $(pwd)" -v -ba --with orchestrator SPECS/contour.spec \
    && rpmbuild --define "_topdir $(pwd)" -v -ba --with daemon --with orchestrator SPECS/contour.spec
