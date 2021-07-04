FROM archlinux/archlinux:base-devel

WORKDIR /app

RUN pacman -Syu --noconfirm --noprogressbar git

RUN useradd -d /app builder
RUN echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
USER builder

RUN sudo chown builder:builder .

RUN git clone https://aur.archlinux.org/contour-git.git .

RUN makepkg -sf --noconfirm --needed && mv $(find . -regex '.*\.\(zst\)') contour.pkg.tar.zst
