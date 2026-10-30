---
type: practice
title: Compile QML to bytecode to avoid private Qt ABI pins
description: >-
  Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API
  and pins the package to one exact Qt release.
tags:
  - packaging
  - qml
  - qt
  - abi
  - debian
kk_schema_version: 3
kk_id: practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
kk_derived_from: []
kk_relates_to:
  - map-per-base-debian-artifacts-and-container-inspection
  - map-branch-truce-desktop-implementation-baseline
kk_depends_on: []
kk_confidence: high
---
[src/ui/CMakeLists.txt](../../../src/ui/CMakeLists.txt) sets `QT_QMLCACHEGEN_ARGUMENTS` to `--only-bytecode` on `branch_truce_ui`. Keep it. The executable then embeds the QML as bytecode and the QML engine runs the bindings.

By default `qmlcachegen` also compiles bindings and functions to C++. That C++ can call private Qt APIs carrying the `Qt_6_PRIVATE_API` symbol version. Qt keeps private API stable only within one release, so native packages that use it require an exact private-ABI dependency or risk failing after a Qt update. Bytecode-only compilation avoids those private symbols. See the [packaging rationale](../../../docs/target-platforms.md#qml-bytecode-instead-of-compiled-c).

[tools/inspect-artifact.sh](../../../tools/inspect-artifact.sh) fails a binary that has `Qt_6_PRIVATE_API` symbols unless `Depends` pins a Qt private ABI to an exact version. Code that brings private Qt symbols back fails that check on Tuxedo, where `dpkg-shlibdeps` produces no pin, and on Kubuntu it ties the package to one Qt release.

<!-- kk:related:start -->
# Related

- Related: [map-per-base-debian-artifacts-and-container-inspection](map-per-base-debian-artifacts-and-container-inspection.md)
- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
<!-- kk:related:end -->
