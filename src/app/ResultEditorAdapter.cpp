#include "ResultEditorAdapter.h"
#include "SessionModel.h"
#include "TextChange.h"
#include "core/ResourceLimits.h"
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QScopedValueRollback>
#include <QTextDocument>
#include <algorithm>
#include <limits>
#include <string_view>

namespace branch_truce {
namespace {
QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QTextDocument *textDocument(QObject *editor)
{
    auto *quick = editor ? editor->property("textDocument").value<QQuickTextDocument *>() : nullptr;
    return quick ? quick->textDocument() : nullptr;
}

// A TextArea attached with TextArea.flickable sits in the Flickable's contentItem,
// so the scrolling item is an ancestor rather than the direct parent.
QQuickItem *scrollViewport(QObject *editor)
{
    auto *item = qobject_cast<QQuickItem *>(editor);
    for (auto *parent = item ? item->parentItem() : nullptr; parent; parent = parent->parentItem())
        if (parent->property("contentX").isValid() && parent->property("contentY").isValid()) return parent;
    return nullptr;
}

QString documentText(const QTextDocument &document)
{
    return document.toRawText().replace(QChar(0x2029), QLatin1Char('\n'));
}

QString envelopeRejection(std::string_view bytes)
{
    const auto assessment = assessText(bytes);
    if (assessment.limit == TextLimit::Within) return {};
    return QString::fromStdString(textLimitDiagnostic(assessment))
        + QStringLiteral(". The existing edit was kept and this change was not acknowledged.");
}

QString envelopeRejection(const QString &text)
{
    const auto bytes = text.toUtf8();
    return envelopeRejection(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}
}

ResultEditorAdapter::ResultEditorAdapter(SessionModel &files, std::function<QString()> selectedFileId,
                                         Placement placement, QObject *parent)
    : QObject(parent), files_(files), selectedFileId_(std::move(selectedFileId)), placementFor_(std::move(placement)),
      words_(files, [this] { return selectedFileId_(); })
{
}

const FileSession *ResultEditorAdapter::selectedFile() const
{
    return files_.file(selectedFileId_().toStdString());
}

void ResultEditorAdapter::attach(QObject *editor)
{
    disconnect(documentConnection_);
    for (const auto &connection : stateConnections_) disconnect(connection);
    stateConnections_.clear();
    if (editor_) editor_->removeEventFilter(this);
    editor_ = editor;
    if (editor) {
        editor->installEventFilter(this);
        stateConnections_.push_back(connect(editor, &QObject::destroyed, this, [this] {
            editable_ = false;
            Q_EMIT stateChanged();
        }));
        // The selection and the paste offer reach the toolbar and the menu through stateChanged.
        const auto changed = QMetaMethod::fromSignal(&ResultEditorAdapter::stateChanged);
        for (const char *name : {"selectedText", "canPaste"}) {
            const auto property = editor->metaObject()->property(editor->metaObject()->indexOfProperty(name));
            if (property.hasNotifySignal())
                stateConnections_.push_back(connect(editor, property.notifySignal(), this, changed));
        }
    }
    document_ = textDocument(editor);
    if (!document_) {
        editable_ = false;
        Q_EMIT stateChanged();
        return;
    }
    document_->setUndoRedoEnabled(false);
    syntax_.attach(document_);
    documentConnection_ = connect(document_, &QTextDocument::contentsChange, this, &ResultEditorAdapter::capture);
    words_.attach(editor, document_);
    sync();
    Q_EMIT stateChanged();
}

void ResultEditorAdapter::attachSource(QObject *editor)
{
    auto *document = textDocument(editor);
    syntax_.attach(document);
    words_.attach(editor, document);
}

void ResultEditorAdapter::setSyntaxFileName(const QString &fileName)
{
    syntax_.setFileName(fileName);
}

void ResultEditorAdapter::setSyntaxBackground(const QColor &background)
{
    syntax_.setBackground(background);
}

bool ResultEditorAdapter::composing() const
{
    return editor_ && editor_->property("inputMethodComposing").toBool();
}

bool ResultEditorAdapter::hasSelection() const
{
    return editor_ && !editor_->property("selectedText").toString().isEmpty();
}

bool ResultEditorAdapter::canPaste() const
{
    return editor_ && editor_->property("canPaste").toBool();
}

void ResultEditorAdapter::setEditable(bool editable)
{
    if (editable_ == editable) return;
    editable_ = editable;
    Q_EMIT stateChanged();
}

ResultEditorAdapter::Caret ResultEditorAdapter::caret() const
{
    if (!editor_) return {};
    return {editor_->property("cursorPosition").toInt(), editor_->property("selectionStart").toInt(),
            editor_->property("selectionEnd").toInt()};
}

bool ResultEditorAdapter::select(int anchor, int cursor)
{
    return editor_ && QMetaObject::invokeMethod(editor_, "select", Qt::DirectConnection,
        Q_ARG(int, anchor), Q_ARG(int, cursor));
}

void ResultEditorAdapter::setCursorPosition(int cursor)
{
    if (editor_) editor_->setProperty("cursorPosition", cursor);
}

void ResultEditorAdapter::capture()
{
    if (syncing_ || processingEvent_ || !document_) return;
    const auto next = documentText(*document_);
    if (next == text_) return;
    if (const auto rejection = envelopeRejection(next); !rejection.isEmpty()) {
        sync();
        Q_EMIT notice(rejection);
        return;
    }
    const auto *file = selectedFile();
    const auto old = text_;
    if (!file || old != text(file->current().draft.text)
        || std::max(old.size(), next.size()) > std::numeric_limits<int>::max()) {
        Q_EMIT editRejected();
        sync();
        return;
    }
    // Observe the complete native event, including a selection replacement or
    // an IME commit with several document notifications, as one domain command.
    const auto change = changedRange(old, next);
    text_ = next;
    Q_EMIT edited(selectedFileId_(), QString::number(file->current().id), static_cast<int>(change.begin),
        static_cast<int>(change.oldEnd), next.mid(change.begin, change.newEnd - change.begin));
    sync();
}

bool ResultEditorAdapter::clipboard(ClipboardAction action)
{
    if (!editor_ || processingEvent_) return false;
    const char *method = nullptr;
    if (action == ClipboardAction::Copy) method = "copy";
    else if (action == ClipboardAction::SelectAll) method = "selectAll";
    else if (action == ClipboardAction::Cut) method = "cut";
    else if (action == ClipboardAction::Paste) method = "paste";
    else return false;
    if ((action == ClipboardAction::Cut || action == ClipboardAction::Paste)
        && (!editable_ || composing()))
        return false;
    if (action == ClipboardAction::Paste) {
        const auto clip = QGuiApplication::clipboard()->text();
        const auto selection = caret();
        const int from = std::clamp(std::min(selection.selectionStart, selection.selectionEnd), 0,
            static_cast<int>(text_.size()));
        const int to = std::clamp(std::max(selection.selectionStart, selection.selectionEnd), 0,
            static_cast<int>(text_.size()));
        if (const auto rejection = envelopeRejection(text_.left(from) + clip + text_.mid(to)); !rejection.isEmpty()) {
            Q_EMIT notice(rejection);
            return false;
        }
    }
    if (auto *item = qobject_cast<QQuickItem *>(editor_.data())) item->forceActiveFocus(Qt::ShortcutFocusReason);
    bool invoked = false;
    {
        QScopedValueRollback guard(processingEvent_, true);
        invoked = QMetaObject::invokeMethod(editor_, method, Qt::DirectConnection);
    }
    capture();
    return invoked && !files_.hasPendingRecovery();
}

bool ResultEditorAdapter::eventFilter(QObject *watched, QEvent *event)
{
    if (syncing_ || processingEvent_) return QObject::eventFilter(watched, event);
    if (watched == editor_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool newline = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        const auto modifiers = key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        // Backspace and Delete carry a control character in text() but remove
        // text. Predicting that character as an insertion rejects a deletion
        // when the line or buffer is already exactly at its limit.
        const bool deletion = key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete;
        if (modifiers == Qt::NoModifier && !deletion && (newline || !key->text().isEmpty())) {
            const auto selection = caret();
            const int from = std::clamp(std::min(selection.selectionStart, selection.selectionEnd), 0,
                static_cast<int>(text_.size()));
            const int to = std::clamp(std::max(selection.selectionStart, selection.selectionEnd), 0,
                static_cast<int>(text_.size()));
            const auto insertion = newline ? QString(QLatin1Char('\n')) : key->text();
            if (const auto rejection = envelopeRejection(text_.left(from) + insertion + text_.mid(to));
                !rejection.isEmpty()) {
                event->accept();
                Q_EMIT notice(rejection);
                return true;
            }
        }
    }
    if (watched == editor_ && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto *key = static_cast<QKeyEvent *>(event);
        QString action;
        if (key->matches(QKeySequence::Undo)) action = QStringLiteral("undo");
        else if (key->matches(QKeySequence::Redo)) action = QStringLiteral("redo");
        else if (key->matches(QKeySequence::Copy)) action = QStringLiteral("copy");
        else if (key->matches(QKeySequence::Paste)) action = QStringLiteral("paste");
        if (!action.isEmpty()) {
            event->accept();
            if (event->type() == QEvent::KeyPress) Q_EMIT editActionRequested(action);
            return true;
        }
    }
    const bool middlePaste = (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
        && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton;
    if (watched == editor_ && (event->type() == QEvent::KeyPress || event->type() == QEvent::InputMethod
        || event->type() == QEvent::Drop || middlePaste)) {
        {
            QScopedValueRollback guard(processingEvent_, true);
            QCoreApplication::sendEvent(watched, event);
        }
        capture();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void ResultEditorAdapter::sync()
{
    if (!document_ || !editor_ || syncing_) return;
    const auto *file = selectedFile();
    const auto *pending = file ? files_.pendingDraft(file->id()) : nullptr;
    const auto stored = file ? (pending ? pending->text : file->current().draft.text) : std::string{};
    if (file && !withinTextEnvelope(stored)) {
        const auto rejection = QString::fromStdString(textLimitDiagnostic(assessText(stored)))
            + QStringLiteral(". The captured text was kept and was not loaded into the editor.");
        syncing_ = true;
        document_->setPlainText({});
        text_.clear();
        setEditable(false);
        syncing_ = false;
        words_.refresh();
        Q_EMIT loadRefused(rejection);
        if (placedFileId_ != selectedFileId_()) place(nullptr, {});
        return;
    }
    const auto desired = file ? text(stored) : QString{};
    const auto shown = documentText(*document_);
    // Undo, redo or a choice replaces the text of the file already shown.
    const auto kept = file && shown != desired && placedFileId_ == selectedFileId_() ? location() : Location{};
    syncing_ = true;
    if (shown != desired) document_->setPlainText(desired);
    text_ = desired;
    const auto loaded = documentText(*document_);
    const bool lossless = loaded == desired && !desired.contains(QChar(0x2028))
        && !desired.contains(QChar(0x2029)) && !desired.contains(QLatin1Char('\r'));
    setEditable(file && file->support() == Support::Supported && lossless && !files_.hasPendingRecovery()
        && !files_.hasRecoveryFailure(file->id()));
    if (!lossless && file && file->support() == Support::Supported)
        Q_EMIT notice(tr("This text cannot be edited losslessly by the current editor. Its captured result is retained."));
    syncing_ = false;
    // Revisions and selections reach the editor here, so word ranges follow them.
    words_.refresh();
    // A new selection, or a loading placeholder replaced by its supported file.
    const bool supported = file && file->support() == Support::Supported;
    if (placedFileId_ != selectedFileId_() || placedSupported_ != supported) place(file, desired);
    else if (kept.valid) keepPlace(kept, shown, loaded);
}

// The viewport goes back first, so the cursor moves it only when it would
// otherwise leave the view.
void ResultEditorAdapter::keepPlace(const Location &place, const QString &before, const QString &after)
{
    if (auto *viewport = place.hasViewport ? scrollViewport(editor_.data()) : nullptr) {
        viewport->setProperty("contentX", place.contentX);
        viewport->setProperty("contentY", place.contentY);
    }
    const auto changes = changedRanges(before, after);
    const int anchor = place.cursor == place.selectionStart ? place.selectionEnd : place.selectionStart;
    (void)select(changedOffset(changes, anchor), changedOffset(changes, place.cursor));
}

// Replacing the document text leaves the cursor at its end, and the result
// viewport scrolls to keep the cursor in view. The file already shown keeps
// its place through keepPlace. A newly selected file instead puts the cursor
// where placementFor_ says, or at the start.
void ResultEditorAdapter::place(const FileSession *file, const QString &loaded)
{
    placedFileId_ = selectedFileId_();
    placedSupported_ = file && file->support() == Support::Supported;
    auto placement = placementFor_(placedSupported_ ? file : nullptr, loaded);
    const int begin = placement.value(QStringLiteral("utf16Begin")).toInt();
    setCursorPosition(begin >= 0 ? begin : 0);
    placement_ = std::move(placement);
    Q_EMIT placed();
}

ResultEditorAdapter::Location ResultEditorAdapter::location() const
{
    Location location;
    if (!editor_) return location;
    location.valid = true;
    location.fileId = selectedFileId_();
    location.text = text_;
    const auto selection = caret();
    location.cursor = selection.cursor;
    location.selectionStart = selection.selectionStart;
    location.selectionEnd = selection.selectionEnd;
    if (auto *viewport = scrollViewport(editor_.data())) {
        location.hasViewport = true;
        location.contentX = viewport->property("contentX").toReal();
        location.contentY = viewport->property("contentY").toReal();
    }
    return location;
}

void ResultEditorAdapter::holdLocation()
{
    heldLocation_ = location();
}

void ResultEditorAdapter::restoreLocation()
{
    if (!heldLocation_.valid || !editor_ || heldLocation_.fileId != selectedFileId_()) return;
    if (text_ != heldLocation_.text) return;
    const auto *file = selectedFile();
    const auto *pending = file ? files_.pendingDraft(file->id()) : nullptr;
    const auto current = file ? text(pending ? pending->text : file->current().draft.text) : QString{};
    if (current != heldLocation_.text) return;
    // select() places the cursor at the second offset and keeps that range.
    const int anchor = heldLocation_.cursor == heldLocation_.selectionStart
        ? heldLocation_.selectionEnd : heldLocation_.selectionStart;
    (void)select(anchor, heldLocation_.cursor);
    if (!heldLocation_.hasViewport) return;
    if (auto *viewport = scrollViewport(editor_.data())) {
        const auto x = heldLocation_.contentX;
        const auto y = heldLocation_.contentY;
        const auto apply = [viewport, x, y] {
            viewport->setProperty("contentX", x);
            viewport->setProperty("contentY", y);
        };
        apply();
        // The relayout after the toggle can clamp the viewport again, so reapply once it has been polished.
        if (auto *window = viewport->window())
            connect(window, &QQuickWindow::afterAnimating, viewport, apply, Qt::SingleShotConnection);
    }
}
}
