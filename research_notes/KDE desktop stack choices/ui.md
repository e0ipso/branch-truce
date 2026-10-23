# KDE desktop UI stack choices

Research date: 2026-10-23. Stack UNDECIDED. Rust backend is the user's preference; bridge and editor remain choices. Research only. Read AGENTS.md, kenkeep ENTRY and relevant leaves, and PRD.md; PRD unchanged. Repository requirements govern recommendations below. Primary-source links state what they support. Unversioned API documentation is not evidence of the latest released version.

## What roles do the UI technologies play?

### Takeaway

KDE recommends Qt Quick with Kirigami for new applications. Branch Truce's editor requirements can still justify Widgets; this is a conditional recommendation, not a selected stack.

### Cited findings

- Qt 6 supplies the application/UI platform. QML is the declarative UI language; Qt Quick supplies visual items, input and rendering; Qt Quick Controls supplies controls such as buttons and menus. Qt also identifies Widgets as suitable for complex traditional desktop applications. [Qt UI overview](https://doc.qt.io/qt-6/topics-ui.html).
- KDE Frameworks are modular Qt add-on libraries. Kirigami adds higher-level, adaptable Qt Quick Controls components. HIG defines interaction/design guidance rather than an implementation library. KDE explicitly recommends Quick plus Kirigami for new apps while recognizing Widgets' suitability for complex desktop interfaces. [Frameworks](https://kde.org/products/frameworks/), [KDE technology guidance](https://develop.kde.org/hig/kde_app_design/), [HIG scope](https://develop.kde.org/hig/).
- Breeze supplies application styling, colors and related visual assets; Breeze Icons is separate. For desktop Quick apps, QQC2 Desktop Style draws through QStyle. Its README recommends it for desktop, requires QApplication, and documents automatic Plasma selection when Breeze and Plasma Integration are installed. This differs from the separate QQC2 Breeze Style. [Breeze components](https://raw.githubusercontent.com/KDE/breeze/Plasma/6.3/README.md), [desktop style contract](https://raw.githubusercontent.com/KDE/qqc2-desktop-style/v6.19.0/README.md).
- Rust integration is documented by KDE using CXX-Qt. CXX-Qt generates QObject bridges rather than automatically wrapping every Qt/KDE class. KDE's Frameworks Rust bindings cover some libraries and explicitly warn of possible changes; their tutorial does not establish KTextEditor bindings. [Rust setup](https://develop.kde.org/docs/getting-started/kirigami/setup-rust/), [CXX-Qt design](https://kdab.github.io/cxx-qt/book/), [KDE bindings scope](https://develop.kde.org/docs/getting-started/rust/rust-bindings/).

### Inferences and recommendations

Recommend a Rust-owned merge/session/decision/recovery core, with a narrow Qt adapter through CXX-Qt and a KDE-native UI. Keep future platform UIs loosely coupled; promise no shared UI code. Prefer Quick/Kirigami if its editor passes the gate below. A Widgets/KTextEditor UI can retain Rust as the backend, but likely needs a maintained C++ adapter. AI maintenance does not remove that integration cost. Tutorials demonstrate supported approaches, not popularity.

### Gaps

Bridge version, Rust toolchain floor, binding coverage and acceptable C++ adapter size remain unselected and untested.

## Can the editor meet the merge experience?

### Takeaway

KTextEditor offers substantial editor functionality as a QWidget. Plain QML text controls are foundations requiring merge-specific engineering. Neither route establishes feasibility without a prototype.

### Cited findings

- KTextEditor Document creates QWidget views, supports read-only operation, text edits, grouped EditingTransaction undo and document recovery. View exposes coordinate conversion, scrolling, annotations and wrapping configuration. [Baseline Document header](https://raw.githubusercontent.com/KDE/ktexteditor/v6.24.0/src/include/ktexteditor/document.h), [View header](https://raw.githubusercontent.com/KDE/ktexteditor/v6.24.0/src/include/ktexteditor/view.h). MovingRange supports additional range highlighting, useful for word-level differences. [MovingRange API](https://api.kde.org/ktexteditor-movingrange.html).
- QQuickWidget embeds Quick into Widgets, not arbitrary Widgets into Quick; it adds rendering overhead and disables threaded rendering. QML WindowContainer accepts QWindow, with stacking restrictions, and is not a general QWidget-to-QML solution. [QQuickWidget](https://doc.qt.io/qt-6/qquickwidget.html), [WindowContainer](https://doc.qt.io/qt-6/qml-qtquick-windowcontainer.html).
- TextArea decorates TextEdit and requires a scrolling container. TextEdit offers selection, read-only mode, wrapping, position mapping and undo. QQuickTextDocument exposes QTextDocument; replacing it is supported since Qt 6.7. These are not documented three-way merge/alignment APIs. [TextArea](https://doc.qt.io/qt-6/qml-qtquick-controls-textarea.html), [TextEdit](https://doc.qt.io/qt-6.8/qml-qtquick-textedit.html), [document access](https://doc.qt.io/qt-6.8/qquicktextdocument.html).
- KSyntaxHighlighting has a real QML SyntaxHighlighter, backed by QTextDocument, and AbstractHighlighter supports custom rendering integrations. It supplies syntax classification, not editing, correspondence or merge decisions. [QML API](https://api.kde.org/qml-org-kde-syntaxhighlighting-syntaxhighlighter.html), [implementation](https://raw.githubusercontent.com/KDE/syntax-highlighting/v6.13.0/src/quick/kquicksyntaxhighlighter.cpp), [custom integration](https://api.kde.org/ksyntaxhighlighting-abstracthighlighter.html).
- QPlainTextEdit provides block geometry and extra selections, making it another Widgets foundation. A custom text engine can use QTextLayout but must supply editor behavior. [QPlainTextEdit](https://doc.qt.io/qt-6/qplaintextedit.html), [QTextLayout](https://doc.qt.io/qt-6.8/qtextlayout.html).

### Inferences and recommendations

Require an editor prototype before choosing the shell:

1. Two read-only source documents surround the editable result; base appears on demand without losing work.
2. Correspondence remains understandable with unequal insertions, repeated text, long wrapped lines, resizing and font changes. Equal scrollbar positions do not prove alignment. Never insert fake padding into saved/copied text.
3. Word highlights coexist with syntax, selection, search and whitespace display. Line/region controls hit the correct contribution.
4. Undo/redo restores text AND decisions, including Ignore with no text change, typing after acceptance, combinations and whole-file replacement. Native text undo alone is insufficient; prove one coherent history across shortcuts and menus.
5. File switching and recovery preserve unsaved edits and outstanding decisions. Editor modified/recovery flags must not substitute for saved-versus-resolved session state.

Prefer Widgets plus KTextEditor if reuse passes these checks more reliably. Prefer Quick when a Quick editor passes without fragile embedding. Treat a custom text engine as a separate substantial undertaking.

### Gaps

No verified public KTextEditor alignment-spacer or arbitrary decision-command undo integration was established. Wrapped-row alignment, performance limits, Rust/editor ownership and recovery coordination remain prototype questions.

## What baseline and integration constraints apply?

### Takeaway

Target Kubuntu 26.04 LTS; Tuxedo compatibility remains unverified.

### Cited findings

- Kubuntu identifies 26.04 as latest LTS, originally released 2026-04-23 with Qt 6.10.2, KF 6.24.0 and Plasma 6.6. Current point media is 26.04.1, announced 2026-08-27. [Latest-LTS listing](https://kubuntu.org/download/), [release stack/date](https://kubuntu.org/news/kubuntu-26-04-release-notes/), [point release](https://lists.ubuntu.com/archives/ubuntu-announce/2026-August/000326.html).
- Plasma desktop archive versions distinguish original 6.6.4 from updates 6.6.6. KF 6.24 Kirigami and KTextEditor require Qt >=6.8 and CMake >=3.27 when building those frameworks; these are component floors, not Branch Truce's promised support floor. [Original package](https://packages.ubuntu.com/resolute/kde/plasma-desktop), [updates](https://packages.ubuntu.com/resolute-updates/kde/plasma-desktop), [Kirigami build requirements](https://raw.githubusercontent.com/KDE/kirigami/v6.24.0/CMakeLists.txt), [KTextEditor requirements](https://raw.githubusercontent.com/KDE/ktexteditor/v6.24.0/CMakeLists.txt).
- TUXEDO's July announcement reports Qt 6.10.2/KF 6.24.0 and mixed Plasma 6.6.4/6.6.5. This does not verify the user's installation or binary compatibility. [Vendor announcement](https://www.tuxedocomputers.com/en/TUXEDO-OS-receives-update-to-KDE-Plasma-6-6-x.tuxedo).
- Qt supplies Wayland clients and HiDPI support; custom drawing still needs correct scaling. Accessibility requires metadata and testing, including Orca. [Wayland](https://doc.qt.io/qt-6/wayland-and-qt.html), [HiDPI](https://doc.qt.io/qt-6/highdpi.html), [KDE accessibility guidance](https://develop.kde.org/hig/accessibility/).
- QML deployment needs its modules/resources. Flatpak's KDE runtime includes Qt/KF, while sandbox permissions constrain repository access. [QML deployment](https://doc.qt.io/qt-6/qtquick-deployment.html), [runtimes](https://docs.flatpak.org/en/latest/available-runtimes.html), [permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html).

### Inferences and recommendations

Use Kubuntu 26.04.1 plus updates as the first verification environment. Test Wayland, mixed/fractional scaling, IME, clipboard, focus, themes, accessible pane/action labels and packaged QML/style plugins. Package on a compatible base; matching Qt/KF versions alone does not prove Tuxedo compatibility.

### Gaps

Unresolved: exact Tuxedo edition/packages/base, native package versus Flatpak, runtime branch, older-distro support, real-file size limits and accessibility acceptance level. No implementation or compatibility test was performed.
