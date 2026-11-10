# Tuxedo OS 24.04: Ubuntu noble with Tuxedo's vendor Qt 6.10 and KDE
# Frameworks 6.24 (ubuntu-plasma). The validation target tuxedo of
# packaging/validation-targets.tsv installs the Kubuntu release DEB here.
# Build context: packaging/.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS target
ENV LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive
# Change to fetch the archives again instead of reusing cached layers.
ARG BRANCH_TRUCE_KDE_CI_REFRESH=
# The Tuxedo repositories use HTTPS, so fetch ca-certificates from the stock
# noble archive first, then use only the Tuxedo OS sources.
RUN apt-get update -q && apt-get install -y -q --no-install-recommends ca-certificates \
    && rm /etc/apt/sources.list.d/ubuntu.sources && rm -rf /var/lib/apt/lists/*
# Key fingerprint E5D0C320BBCE8D21CDF60DD5120ED28D54840598.
COPY tuxedo/tuxedo-archive-keyring.asc /etc/apt/keyrings/
COPY tuxedo/tuxedo-os.sources /etc/apt/sources.list.d/
RUN apt-get update -q && apt-get dist-upgrade -y -q --no-install-recommends
