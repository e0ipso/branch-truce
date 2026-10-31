# Source layout

The desktop app separates the Qt-free merge engine from repository, storage and window code. Choose a directory by the responsibility of the new file.

| Directory | Responsibility | Put new files here when they… |
| --- | --- | --- |
| [app/](app/README.md) | Startup, controller and Qt-facing models | translate window commands or editor state into session operations |
| [core/](core/README.md) | Merge state, correspondence and revision history | define platform-independent values or editing rules |
| [git/](git/README.md) | Repository discovery, classification and bounded Git operations | inspect or change Git state through named operations |
| [highlighting/](highlighting/README.md) | Syntax colours and word decorations | format comparison documents |
| [mergetool/](mergetool/README.md) | Git launcher, local protocol and parent handoff | connect a waiting Git mergetool process to the app |
| [publication/](publication/README.md) | Reviewed result publication | coordinate destination writes with verified staging or handoff |
| [recovery/](recovery/README.md) | Acknowledgment, replay and interrupted-mutation reconciliation | retain or restore unfinished session work |
| [session/](session/README.md) | Conflict capture and background operation coordination | assemble snapshots or schedule guarded session work |
| [storage/](storage/README.md) | Text formats, destination writes and journal storage | implement filesystem and serialization mechanisms |
| [ui/](ui/README.md) | QML components and presentation helpers | render or operate the comparison window |

Each directory's README names its entry points and nearby boundaries. Keep QML free of Git and file writes, and keep Qt, subprocesses and filesystem handles out of core/. Each subsystem declares its target in CMakeLists.txt; the root CMakeLists.txt orders them and app/ includes the UI target.
