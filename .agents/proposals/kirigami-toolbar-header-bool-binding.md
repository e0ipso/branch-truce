# Kirigami toolbar header warning: reproduction not established

The application test log `/tmp/truce275-lasttest.log` contains 52 instances of
`ToolBarPageHeader.qml:63:13: Unable to assign [undefined] to bool` from
`qrc:/qt/qml/org/kde/kirigami/controls/private/globaltoolbar/ToolBarPageHeader.qml`.
The producing attempt was `event.RguPCLyTnk`. Those warnings occurred amid other
application and engine teardown diagnostics, so their trigger is not established.
Do not submit an upstream bug report on this evidence alone.

On the native Tuxedo OS 24.04 environment, the corresponding installed source is
`/usr/lib/x86_64-linux-gnu/qt6/qml/org/kde/kirigami/controls/private/globaltoolbar/ToolBarPageHeader.qml`,
owned by `qml6-module-org-kde-kirigami:amd64` version `6.24.0-0ubuntu1~tux1`.
Its SHA-256 is `d4a1833ee3028fc74f87910e5832c4df6d880e4711dd5e90e37781cab1da34cc`.
Qt's `libqt6qml6:amd64` version is `6.10.2+dfsg-3~tux1`. The runtime warning
names a qrc resource, so the file's package ownership alone does not prove the
runtime resource bytes match this installed copy.

Line 63 assigns the result of `root.pageRow?.separatorVisible && ...` to
`visible`. JavaScript short-circuit evaluation can return `undefined` when
`pageRow` is absent. Kirigami's `Page.qml` binds the header's `pageRow` to a
row that can become null on a parent change. This is a plausible mechanism,
not evidence that it caused the application warning. The application uses
`Kirigami.ApplicationWindow` and `pageStack.initialPage` in `src/ui/Main.qml`;
no misuse has been established either.

## Native attempt

From the repository root run:

```sh
bash .agents/proposals/kirigami-toolbar-header-repro.sh
```

The script uses CMake and Qt 6 development libraries to build a disposable
program under `/tmp`, creates ten `QQmlApplicationEngine` instances in turn,
loads a visible Kirigami window with a page and explicit toolbar style, processes
events once, then destroys each engine. It uses the offscreen Qt platform and
`org.kde.desktop` controls. It deletes its temporary source and build directory.
The observed run completed all ten cycles with zero matching warnings; raw
output is `/tmp/sift-check-evidence/check.EzdU3SMFK2/raw.log`. A separate
`qmltestrunner` window creation, clear and teardown attempt also produced zero
matching warnings in `/tmp/sift-check-evidence/check.yUkkLmdCQD/raw.log`.
These results do not rule out a lifecycle-dependent upstream defect, project
misuse, or a difference between the resource and installed file.

To isolate the remaining cause, instrument the application's affected window
and page lifecycle to identify when the header's `pageRow` becomes null, then
reproduce that sequence with public Kirigami APIs and confirm the resource's
source and version. If confirmed, a candidate is to give the optional separator
lookup a boolean fallback, for example `(root.pageRow?.separatorVisible ??
false) && ...`. That change has not been applied or tested. Check normal
separator visibility and page transitions before proposing it upstream. No
installed dependencies or user settings were modified; offscreen execution does
not validate a Plasma desktop.
