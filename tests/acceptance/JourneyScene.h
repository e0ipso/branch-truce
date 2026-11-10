#pragma once

// What every journey stands on: its Sandbox, the real Main scene over a controller that opened a
// fixture repository (Scene), typed readers for the controller's QVariantMap projections, and
// the event-loop waits. A reader fails the step when a key it reads is absent, so a renamed key
// cannot turn into a false or 0 that a negative assertion accepts.

#include "TestHarness.h"
#include "app/Application.h"
#include "fixtures/ConflictedRepository.h"
#include "fixtures/GitRepository.h"

#include <QMap>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace journey {

// One path of a journey's repository, by its base, left and right bytes; a missing side deletes the
// path on that branch. Left is main, the branch checked out when a merge stops, or the rebased
// state; right is side, the branch merged or the commit replayed.
struct PathSpec {
    QString path;
    std::optional<QByteArray> base;
    std::optional<QByteArray> left;
    std::optional<QByteArray> right;
    // Written over Git's conflict output after the operation stops, as a user would before launch.
    std::optional<QByteArray> manual;
};

// A repository stopped on `paths` by a real merge or rebase, with a tracked README and an
// unreachable origin remote that opening and resolving must not contact.
class Repository : public test_support::ConflictedRepository {
public:
    explicit Repository(const std::vector<PathSpec> &paths, Stop stop = Stop::Merge);
    // The commits on each side, full object ids.
    const QByteArray &leftCommit() const { return leftCommit_; }
    const QByteArray &rightCommit() const { return rightCommit_; }
    QByteArray stageObject(int stage, const QString &relative) const;
    QByteArray stageText(int stage, const QString &relative) const;

private:
    QByteArray leftCommit_;
    QByteArray rightCommit_;
};

// The id the session gives a path: its bytes in hex.
QString fileId(const char *path);

// Runs the event loop until ready() holds. Fails with `what` and state() after the journey
// deadline, test_support::kWaitDeadlineMs, which covers a Git process or a launcher attaching.
void waitFor(const std::string &what, const std::function<bool()> &ready,
             const std::function<std::string()> &state = {});
// Runs the event loop until ready() holds or `deadlineMs` pass, and returns whether it held.
bool heldWithin(const std::function<bool()> &ready, int deadlineMs);
// What one journey owns, so no journey sees another's state: an offline Git guard that logs the
// app's Git calls, a recovery root that holds the journey's sessions, and an owner-only runtime
// directory for the mergetool socket. Every Scene of the journey shares it, as successive windows
// on one machine share the data directory.
class Sandbox {
public:
    Sandbox();
    Sandbox(const Sandbox &) = delete;
    Sandbox &operator=(const Sandbox &) = delete;

    // A window's services for `repository`: the guard as its Git, a new session under this
    // journey's recovery root, the repository's Git configuration and the private socket directory.
    branch_truce::Application::Config config(const Repository &repository) const;
    QString runtimeDirectory() const { return runtime_.path(); }
    QString socketDirectory() const;
    const test_support::OfflineGit &git() const { return git_; }
    // The environment `git mergetool` runs in: the repository's, so the launcher reads the same Git
    // configuration as the window, with the guard first on PATH and this journey's runtime
    // directory, where the launcher finds the window's socket.
    QProcessEnvironment mergetoolEnvironment(const Repository &repository) const;

private:
    test_support::OfflineGit git_;
    QTemporaryDir recovery_;
    test_support::RuntimeDirectory runtime_;
};

// A selected file's progress, from SelectedFile::progress().
struct Progress {
    qulonglong undecided = 0;
    qulonglong conflicts = 0;
    bool saved = false;
    bool reviewed = false;
    bool resolved = false;
    bool earlierResolved = false;
    bool settled = false;
    QString currentRevision;
    QString savedRevision;
    QString reviewedRevision;
    QString stagedRevision;
    bool operator==(const Progress &) const = default;
};

// Lines and UTF-16 positions of a range in one pane; -1 throughout when it is not mapped.
struct PaneRange {
    int lineBegin = -1;
    int lineEnd = -1;
    int utf16Begin = -1;
    int utf16End = -1;
    bool operator==(const PaneRange &) const = default;
};

// One scoped-choice region of the selected file, from SelectedFile::regions().
struct Region {
    QString id;
    QString side;
    QString scope;
    QString section;
    QString state;
    QStringList decisionIds;
    bool handEdited = false;
    bool markersRemain = false;
    bool mapped = false;
    PaneRange source;
    PaneRange result;
    bool operator==(const Region &) const = default;
};

// One row of the scoped-choice list, from SelectedFile::choices().
struct ChoiceRow {
    QString id;
    QString label;
    QString state;
    QString detail;
    bool available = false;
};

// One conflict-marker block of the result, from SelectedFile::markerBlocks().
struct MarkerBlock {
    int resultLineBegin = 0;
    int resultLineEnd = 0;
    bool folded = false;
};

// One file of the handoff, from SessionCompletion::handoffData().
struct HandoffEntry {
    QString category;
    QString actionLabel;
    bool outstanding = false;
    QStringList blockers;
};

// The real Main scene over a controller that opened the repository itself.
class Scene {
public:
    Scene(Sandbox &sandbox, const Repository &repository,
          std::shared_ptr<const branch_truce::storage::FileSystem> files = branch_truce::storage::systemFileSystem());
    ~Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;

    // The window's services, for the actions and states a journey has no control for.
    branch_truce::Application &app() const { return *controller_; }
    QQuickWindow *window() const { return window_; }
    QQmlApplicationEngine &engine() const { return *engine_; }
    const Sandbox &sandbox() const { return sandbox_; }

    // A named item, preferring the visible one; it fails the step when there is none.
    QQuickItem *item(const char *name) const;
    QObject *object(const char *name) const;
    // A label's text, or for a row of labels such as the status chips, their visible texts.
    QString text(const char *name) const;
    // Clicks a named control, scrolling it into view and opening the toolbar overflow first when
    // it is there.
    void click(const char *name);
    // Scroll enclosing views, as a user would, so a clipped target is under the pointer.
    static bool reveal(QQuickItem *target);
    // Positions must hold this long before a click; see test_support::waitForStable.
    static constexpr int settleMs = 60;

    const branch_truce::FileSession &file(const char *path) const;
    Progress progress() const;
    std::vector<Region> regions() const;
    std::vector<ChoiceRow> choiceRows() const;
    ChoiceRow choiceRow(const QString &label) const;
    // The region that shares its ID with one scoped-choice row.
    Region regionFor(const QString &label) const;
    std::vector<MarkerBlock> markerBlocks() const;
    // The handoff's entries by file ID, and whether it is clean.
    QMap<QString, HandoffEntry> handoffEntries() const;
    bool handoffClean() const;

    // The Git calls this window ran: the journey's log after what it held when the window opened.
    QByteArray gitCalls() const;

private:
    Sandbox &sandbox_;
    const qsizetype gitCallsBefore_;
    std::unique_ptr<branch_truce::Application> controller_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QQuickWindow *window_ = nullptr;
};

} // namespace journey
