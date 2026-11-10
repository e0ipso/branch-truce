#pragma once

// The steps journeys take through the window, as a user would: clicks, keys, dialogs and file
// rows, and the checks several journeys share. Every step works on a Scene.

#include "acceptance/JourneyScene.h"
#include "core/Draft.h"
#include "fixtures/mergetool/RealParent.h"

#include <QJsonObject>
#include <QPointF>
#include <QQuickItem>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <string>

namespace journey {

// --- finding things ------------------------------------------------------------------------

// The item find() returns once it is visible and its position has held for Scene::settleMs.
QQuickItem *stable(Scene &scene, const std::string &what, const std::function<QQuickItem *()> &find);
// The first item under root, root included, that match() accepts.
QQuickItem *descendant(QQuickItem *root, const std::function<bool(QQuickItem *)> &match);
QPointF sceneOrigin(QQuickItem *item);
// Every visible text under root, including open dialogs.
void collectTexts(QQuickItem *root, QStringList &texts);
// Every visible text in the window, one per line.
QString shownTexts(Scene &scene);
// A file row's texts and its accessible description, which holds the row's summary.
QString rowText(Scene &scene, const char *path);

// --- reading the result --------------------------------------------------------------------

// Text and decisions together, as shared undo/redo must restore them.
struct Work {
    QString shown;
    branch_truce::Draft draft;
    bool operator==(const Work &) const = default;
};
Work work(Scene &scene, const char *path);
// The displayed result text inside a region's UTF-16 result range.
QString regionResult(Scene &scene, const Region &region);
QString utf8(const QByteArray &bytes);

// --- acting --------------------------------------------------------------------------------

// Hover and click one scoped source overlay. kind is the part of the control's objectName before
// the choice ID: Accept, Ignore, Resolve, PreviewLeftFirst or PreviewRightFirst. menuKey opens the
// source line's context menu with that key instead of the mouse.
void clickChoice(Scene &scene, const QString &label, const char *kind, int menuKey = 0);
// A dialog button by objectName, or a standard button such as OK, Cancel or Apply by its
// accessible name, once the dialog is open.
QQuickItem *dialogButton(Scene &scene, const char *dialog, const QString &button);
// Click a dialog button and wait for the dialog to close.
void clickDialogButton(Scene &scene, const char *dialog, const QString &button);
// Select a file the way a user does: click its row in the file list.
void clickFile(Scene &scene, const char *path);
void keyClick(Scene &scene, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
// Send each Latin-1 character as a key click.
void typeText(Scene &scene, const char *text);
// Click near the top-left of a text area: unwrapped text can extend past its viewport.
void clickText(Scene &scene, QQuickItem *editor);
void appendToResult(Scene &scene, const char *text);
// Mark resolved through its confirmation, then wait for Git to resolve the file.
void markResolvedByMouse(Scene &scene);
// Opens the file toolbar's overflow menu and clicks the entry of the action with this objectName.
void clickOverflowAction(Scene &scene, const char *name);
// A named toolbar control may move to overflow as the window narrows.
void clickFileToolbarControl(Scene &scene, const char *name);
// Opens the file toolbar's overflow menu and clicks its Show conflict markers entry.
void toggleConflictMarkers(Scene &scene);

// --- checking ------------------------------------------------------------------------------

void expectText(const QString &actual, const QString &expected, const char *what);
void expectContains(const QString &actual, const QString &expected, const char *what);
// Offline and account-free: no Git network command ran, this window ran local Git itself, and
// nothing asks for an account.
void checkOffline(Scene &scene);
// Waits until the result folds each conflict-marker block into one row, or shows every line.
void expectMarkersFolded(Scene &scene, bool folded);
// Checks a read-only pane against the result's font and row geometry, selects its first
// characters by keyboard and checks typing changes nothing. Returns what it measured.
QJsonObject inspectReadOnlyPane(Scene &scene, const char *name, const char *header);
// A loader's startup state, its recovery choices and its detail, for a failure message.
std::string loaderState(const branch_truce::SessionLoader &loader);

// --- Git mergetool -------------------------------------------------------------------------

// Real `git mergetool` over the given paths, leading its own process group so a failed journey
// kills the tool command and the launcher with it. This test binary is the launcher: main() runs
// the real launcher code when Git starts it with --mergetool.
std::unique_ptr<mergetool_fixture::ParentProcess> startMergetool(const Sandbox &sandbox, const Repository &repo,
                                                                 const QStringList &paths);

} // namespace journey
