FROM buildpack-deps:disco AS base

ARG DEBIAN_FRONTEND=noninteractive

RUN set -ex; \
	apt-get update; \
	apt-get install -qqy --no-install-recommends \
		build-essential \
		software-properties-common \
		cmake ninja-build clang++-8 g++-9 \
		; \
	apt-get install -qy python-pip; \
	pip install codecov; \
	rm -rf /var/lib/apt/lists/*
