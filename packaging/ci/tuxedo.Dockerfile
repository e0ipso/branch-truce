# Tuxedo OS 24.04 (Ubuntu noble base + Tuxedo vendor Qt/KDE repositories)
# stack for branch-truce.
#
# Build it from packaging/ (packaging/scripts/container-check.sh does):
#   docker build -f ci/tuxedo.Dockerfile --target <stage> .

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS target
ENV LANG=C.UTF-8
ARG BRANCH_TRUCE_KDE_CI_REFRESH=
COPY ci/tuxedo-archive-keyring.gpg /opt/branch-truce/ci/tuxedo-archive-keyring.gpg
COPY scripts/prepare-target.sh /opt/branch-truce/scripts/
RUN /opt/branch-truce/scripts/prepare-target.sh --format tuxedo-deb

FROM target AS build
COPY debian/control /opt/branch-truce/debian/
RUN /opt/branch-truce/scripts/prepare-target.sh --format tuxedo-deb --build-deps
COPY ci/baseline.sh /opt/branch-truce/ci/
RUN /opt/branch-truce/ci/baseline.sh --format tuxedo-deb > /opt/branch-truce/baseline.txt \
    && cat /opt/branch-truce/baseline.txt

FROM build AS test
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
        appstream desktop-file-utils hicolor-icon-theme kde-style-breeze \
        kf6-breeze-icon-theme libasan8 libglib2.0-bin libubsan1 \
        qml6-module-org-kde-desktop qml6-module-org-kde-kirigami \
        qml6-module-qtqml-models qml6-module-qtqml-workerscript \
        qml6-module-qtquick \
        qml6-module-qtquick-controls qml6-module-qtquick-layouts \
        qml6-module-qtquick-shapes qml6-module-qtquick-templates \
        qml6-module-qtquick-window qml6-module-qttest \
        qt6-svg-plugins qt6-wayland
