# Build from packaging/: docker build -f ci/fedora.Dockerfile --target <stage> .
FROM fedora:44@sha256:43b29f65a41eb9c35e1cd5323e3bdf3b655c2357a9f4f1ff2f9c2798e5045d80 AS target
ENV LANG=C.UTF-8
ARG BRANCH_TRUCE_KDE_CI_REFRESH=
# Inspection tools only. Runtime Qt/KDE must come from the RPM's dependencies.
RUN dnf install -y --setopt=install_weak_deps=False \
        binutils coreutils cpio diffutils file findutils gawk gzip grep sed tar util-linux \
        appstream desktop-file-utils \
    && dnf clean all

FROM target AS build
COPY rpm/branch-truce.spec /opt/branch-truce/rpm/
RUN dnf install -y --setopt=install_weak_deps=False rpm-build ccache \
    && dnf builddep -y --setopt=install_weak_deps=False -D 'branch_truce_version 0.0.0' \
        -D 'branch_truce_native_version 0.0.0' /opt/branch-truce/rpm/branch-truce.spec \
    && dnf clean all
COPY ci/baseline.sh /opt/branch-truce/ci/
RUN /opt/branch-truce/ci/baseline.sh --format fedora-rpm > /opt/branch-truce/baseline.txt \
    && cat /opt/branch-truce/baseline.txt

FROM build AS test
RUN dnf install -y --setopt=install_weak_deps=False \
        'cmake(Qt6Test)' 'cmake(Qt6QuickTest)' 'qt6qml(QtTest)' \
        'qt6qml(org.kde.desktop)' 'qt6qml(org.kde.kirigami)' \
        qt6-qtsvg qt6-qtwayland plasma-breeze-qt6 breeze-icon-theme plasma-integration \
        dbus-daemon dbus-tools dpkg jq python3 libasan libubsan \
        sway grim plasma-breeze google-noto-sans-fonts source-foundry-hack-fonts \
    && dnf clean all
