# Kubuntu 26.04 LTS: Ubuntu resolute with its archive's Qt 6.10 and KDE
# Frameworks 6.24, the selected KDE/API baseline. Build context: packaging/.
# Rebuild with tools/build-artifact.sh --target kubuntu.
FROM ubuntu:26.04@sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7 AS target
ENV LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive
# Change to fetch the archive again instead of reusing cached layers.
ARG BRANCH_TRUCE_REFRESH=
RUN apt-get update -q && apt-get dist-upgrade -y -q --no-install-recommends

FROM target AS build
COPY debian/control /opt/branch-truce/debian/
RUN apt-get install -y -q --no-install-recommends build-essential debhelper dpkg-dev \
    && apt-get build-dep -y -q --no-install-recommends /opt/branch-truce/
