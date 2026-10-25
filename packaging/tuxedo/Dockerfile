# Tuxedo OS 24.04: Ubuntu noble with Tuxedo's vendor Qt 6.10 and KDE
# Frameworks 6.24 (ubuntu-plasma). Stock noble's Qt 6.4 cannot build the
# desktop target. Build context: packaging/.
# Rebuild with tools/build-artifact.sh --target tuxedo.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS target
ENV LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive
ARG BRANCH_TRUCE_REFRESH=
# The Tuxedo repositories use HTTPS, so fetch ca-certificates from the stock
# noble archive first, then use only the Tuxedo OS sources.
RUN apt-get update -q && apt-get install -y -q --no-install-recommends ca-certificates \
    && rm /etc/apt/sources.list.d/ubuntu.sources && rm -rf /var/lib/apt/lists/*
# Key fingerprint E5D0C320BBCE8D21CDF60DD5120ED28D54840598.
COPY tuxedo/tuxedo-archive-keyring.asc /etc/apt/keyrings/
COPY tuxedo/tuxedo-os.sources /etc/apt/sources.list.d/
RUN apt-get update -q && apt-get dist-upgrade -y -q --no-install-recommends

FROM target AS build
COPY debian/control /opt/branch-truce/debian/
RUN apt-get install -y -q --no-install-recommends build-essential debhelper dpkg-dev \
    && apt-get build-dep -y -q --no-install-recommends /opt/branch-truce/
