# kenkeep Index: packaging

↑ Parent: [kenkeep](../index.md)

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
- Load [`artifact-paths/`](artifact-paths/index.md) for more information on developer artifacts, native releases and runtime dependency checks; read when changing package construction or artifact inspection.
- Load [`installed-validation/`](installed-validation/index.md) for more information on installed DEB validation execution, evidence limits and tooling checks; read when changing installed-package validation or interpreting container results.

## Conventions (how we build)
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) to learn about: Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release. #packaging #qml #qt #abi #debian

## Components (what exists)
_None yet._

## By topic

### #abi
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) — Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release.
### #debian
- Open [**Native package build entry point**](artifact-paths/map-developer-debian-artifact-builds-and-inspection.md) — One format-driven container entry point runs desktop tests, package construction and clean-target inspection.
- Open [**Native release package artifact flow**](artifact-paths/map-native-release-package-artifact-flow.md) — Release CI validates target-native package bytes and publishes those artifacts through digest manifests.
- Open [**Runtime QML and plugin package dependencies**](artifact-paths/map-runtime-qml-and-plugin-package-dependencies.md) — Imported QML modules, styles and plugins need explicit package dependencies and inspection coverage.
### #packaging
- Open [**Native package build entry point**](artifact-paths/map-developer-debian-artifact-builds-and-inspection.md) — One format-driven container entry point runs desktop tests, package construction and clean-target inspection.
- Open [**Native release package artifact flow**](artifact-paths/map-native-release-package-artifact-flow.md) — Release CI validates target-native package bytes and publishes those artifacts through digest manifests.
- Open [**Runtime QML and plugin package dependencies**](artifact-paths/map-runtime-qml-and-plugin-package-dependencies.md) — Imported QML modules, styles and plugins need explicit package dependencies and inspection coverage.
### #qml
- Open [**Wait for layout before clicking offscreen dialog items**](../ui/practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Literal project text in native controls**](../ui/map-literal-project-text-in-native-controls.md) — File rows and tooltips keep native backing text empty so retained auto-text labels never receive repository markup.
- Open [**Wait for rendered frames before capturing native scenes**](../ui/practice-wait-for-rendered-frames-before-capturing-native-scenes.md) — Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus.
### #qt
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) — Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release.