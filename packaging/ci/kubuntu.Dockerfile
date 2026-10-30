# Kubuntu 26.04 LTS stack for branch-truce.
#
# Build it from packaging/ (packaging/scripts/container-check.sh does):
#   docker build -f ci/kubuntu.Dockerfile --target <stage> .

FROM ubuntu:26.04@sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7 AS target
ENV LANG=C.UTF-8
ARG BRANCH_TRUCE_KDE_CI_REFRESH=
COPY scripts/prepare-target.sh /opt/branch-truce/scripts/
RUN /opt/branch-truce/scripts/prepare-target.sh --format kubuntu-deb \
    && apt-get install -y -q --no-install-recommends appstream desktop-file-utils binutils

FROM target AS build
COPY debian/control /opt/branch-truce/debian/
RUN /opt/branch-truce/scripts/prepare-target.sh --format kubuntu-deb --build-deps
COPY ci/baseline.sh /opt/branch-truce/ci/
RUN /opt/branch-truce/ci/baseline.sh --format kubuntu-deb > /opt/branch-truce/baseline.txt \
    && cat /opt/branch-truce/baseline.txt

FROM build AS test
# app.startup-appearance also exercises the KDE platform theme plugin.
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
        appstream desktop-file-utils hicolor-icon-theme kde-style-breeze \
        kf6-breeze-icon-theme libasan8 libglib2.0-bin libubsan1 \
        plasma-integration \
        qml6-module-org-kde-desktop qml6-module-org-kde-kirigami \
        qml6-module-qtqml-models qml6-module-qtqml-workerscript \
        qml6-module-qtquick \
        qml6-module-qtquick-controls qml6-module-qtquick-layouts \
        qml6-module-qtquick-shapes qml6-module-qtquick-templates \
        qml6-module-qtquick-window qml6-module-qttest \
        qt6-svg-plugins qt6-wayland
