FROM archlinux/archlinux:base-devel

WORKDIR /app

RUN pacman -Syu --noconfirm --noprogressbar git

RUN useradd -d /app builder
RUN echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
USER builder

RUN echo "Reading /etc/os-release" \
    cat /etc/os-release || true

RUN sudo chown builder:builder .

RUN git clone --depth=1 https://github.com/contour-terminal/contour-aur.git .

RUN makepkg -sf --noconfirm --needed && mv $(find . -regex '.*\.\(zst\)') contour.pkg.tar.zst
