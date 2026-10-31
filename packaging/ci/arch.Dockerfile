# Build from packaging/: docker build -f ci/arch.Dockerfile --target <stage> .
FROM archlinux:base@sha256:dfc5be7be44cf1acf3d9b3b902e3e9c8a29f97ed41e35631823cdc9c97e5d4f0 AS target
ENV LANG=C.UTF-8
ARG BRANCH_TRUCE_KDE_CI_REFRESH=
# Always upgrade the complete rolling base before installing packages.
# Repository package signatures retain pacman's default Required policy.
RUN pacman -Syu --noconfirm --needed archlinux-keyring \
        binutils diffutils findutils file desktop-file-utils appstream \
    && pacman -Q > /opt/branch-truce-target-baseline.txt \
    && cat /opt/branch-truce-target-baseline.txt

FROM target AS build
COPY arch/PKGBUILD /opt/branch-truce/arch/
RUN pacman -S --noconfirm --needed base-devel cmake ninja ccache extra-cmake-modules \
        git qt6-base qt6-declarative qt6-svg qt6-wayland kirigami \
        syntax-highlighting qqc2-desktop-style breeze breeze-icons hicolor-icon-theme \
    && useradd --create-home --uid 1000 --groups root builder \
    && pacman -Q > /opt/branch-truce-build-baseline.txt \
    && cat /opt/branch-truce-build-baseline.txt
COPY ci/baseline.sh /opt/branch-truce/ci/
RUN /opt/branch-truce/ci/baseline.sh --format arch-pkg > /opt/branch-truce/baseline.txt \
    && cat /opt/branch-truce/baseline.txt

FROM build AS test
# Startup appearance checks use the Plasma platform plugin; glib2 supplies gio.
RUN pacman -S --noconfirm --needed plasma-integration glib2 python jq sway grim noto-fonts ttf-hack \
    && pacman -Q > /opt/branch-truce-test-baseline.txt \
    && cat /opt/branch-truce-test-baseline.txt
