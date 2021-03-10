FROM archlinux:latest

WORKDIR /app

COPY . .

RUN pacman -Sy --noconfirm archlinux-keyring

RUN pacman -Su --noconfirm gcc ninja cmake extra-cmake-modules qt5-base fontconfig harfbuzz git base-devel

RUN ./.github/archlinux/build
