// Standalone acceptance experiment, deliberately independent of product code.
#include <QClipboard>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSaveFile>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextStream>
#include <optional>

namespace {
int assertions = 0;
int failures = 0;
void check(bool condition, const char *description)
{
    ++assertions;
    if (!condition) {
        ++failures;
        QTextStream(stderr) << "FAIL " << description << '\n';
    }
}
QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems()) {
        if (auto *found = findItem(child, name)) return found;
    }
    return nullptr;
}
QTextDocument *documentFor(QQuickItem *item)
{
    return item->property("textDocument").value<QQuickTextDocument *>()->textDocument();
}

// Entries exist only at Unicode scalar boundaries, including end-of-buffer.
// This intentionally distinguishes scalar boundaries from grapheme navigation.
QString bufferText(QTextDocument *document)
{
    // Unlike toPlainText(), toRawText() preserves NBSP. LF paragraph projection
    // is reversible because literal U+2029 is rejected at this boundary.
    return document->toRawText().replace(QChar(0x2029), QChar(u'\n'));
}

class Positions {
public:
    explicit Positions(const QString &text)
    {
        qsizetype bytes = 0;
        boundaries.append({0, 0});
        for (qsizetype i = 0; i < text.size();) {
            const auto c = text.at(i);
            qsizetype units = 1;
            if (c.isHighSurrogate()) {
                if (i + 1 >= text.size() || !text.at(i + 1).isLowSurrogate()) {
                    valid = false;
                    return;
                }
                units = 2;
            } else if (c.isLowSurrogate()) {
                valid = false;
                return;
            }
            bytes += text.mid(i, units).toUtf8().size();
            i += units;
            boundaries.append({i, bytes});
        }
    }
    std::optional<qsizetype> toByte(qsizetype qt) const { return lookup(qt, false); }
    std::optional<qsizetype> toQt(qsizetype byte) const { return lookup(byte, true); }
    bool valid = true;
private:
    std::optional<qsizetype> lookup(qsizetype value, bool reverse) const
    {
        if (!valid) return std::nullopt;
        for (const auto &[qt, byte] : boundaries) {
            if ((reverse ? byte : qt) == value) return reverse ? qt : byte;
        }
        return std::nullopt;
    }
    QList<QPair<qsizetype, qsizetype>> boundaries;
};
struct State {
    QString text;
    QString left = QStringLiteral("undecided");
    QString right = QStringLiteral("undecided");
    bool operator==(const State &) const = default;
};

// A disposable integrity-checked snapshot sink, NOT the product recovery journal.
class Recovery {
public:
    explicit Recovery(QString destination) : path(std::move(destination)) {}
    bool persist(const State &state)
    {
        QElapsedTimer clock;
        clock.start();
        if (failWrites) return false;
        const auto payload = QJsonDocument(QJsonObject{
            {QStringLiteral("text"), state.text}, {QStringLiteral("left"), state.left},
            {QStringLiteral("right"), state.right}}).toJson(QJsonDocument::Compact);
        const auto record = QJsonDocument(QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("payload"), QString::fromLatin1(payload.toBase64())},
            {QStringLiteral("sha256"), QString::fromLatin1(
                QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())}}).toJson();
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)
            || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || file.write(record) != record.size() || !file.commit()) return false;
        ++writes;
        maxNanos = qMax(maxNanos, clock.nsecsElapsed());
        return true;
    }
    std::optional<State> restore() const
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
        const auto record = QJsonDocument::fromJson(file.readAll()).object();
        const auto payload = QByteArray::fromBase64(record.value(QStringLiteral("payload")).toString().toLatin1());
        if (record.value(QStringLiteral("version")).toInt() != 1
            || record.value(QStringLiteral("sha256")).toString().toLatin1()
                != QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()) return std::nullopt;
        const auto data = QJsonDocument::fromJson(payload).object();
        if (!data.value(QStringLiteral("text")).isString()
            || !data.value(QStringLiteral("left")).isString()
            || !data.value(QStringLiteral("right")).isString()) return std::nullopt;
        return State{data.value(QStringLiteral("text")).toString(),
                     data.value(QStringLiteral("left")).toString(),
                     data.value(QStringLiteral("right")).toString()};
    }
    QString path;
    bool failWrites = false;
    int writes = 0;
    qint64 maxNanos = 0;
};

class Adapter : public QObject {
    Q_OBJECT
public:
    Adapter(QQuickItem *item, QQuickItem *scene, Recovery &sink)
        : editor(item), root(scene), document(documentFor(item)), recovery(sink)
    {
        editor->setProperty("readOnly", true);
        document->setUndoRedoEnabled(false);
        editor->installEventFilter(this);
        connect(editor, SIGNAL(textChanged()), this, SLOT(capture()));
    }
    bool initializeUtf8(const QByteArray &bytes)
    {
        const auto text = QString::fromUtf8(bytes);
        return text.toUtf8() == bytes && initialize(text);
    }
    bool initialize(const QString &text)
    {
        State initial{text};
        if (!supported(text) || !recovery.persist(initial)) {
            status(QStringLiteral("Recovery unavailable; editing blocked"));
            return false;
        }
        history = {initial};
        index = 0;
        render(initial);
        editor->setProperty("readOnly", false);
        status(QStringLiteral("Initial snapshot ready; unsaved draft"));
        return true;
    }
    State state() const { return history.at(index); }
    static bool supported(const QString &text)
    {
        // QTextDocument normalizes paragraph separators and CR; conversion must
        // handle these outside this experiment rather than silently losing them.
        if (!Positions(text).valid || text.contains(QChar(u'\r'))
            || text.contains(QChar(0x2028)) || text.contains(QChar(0x2029))) return false;
        QTextDocument projection;
        projection.setPlainText(text);
        return bufferText(&projection) == text;
    }
    bool replace(qsizetype expectedRevision, qsizetype startByte, qsizetype endByte,
                 const QString &replacement, const QString &left, const QString &right)
    {
        if (expectedRevision != acknowledged || startByte > endByte) return false;
        const Positions positions(state().text);
        const auto start = positions.toQt(startByte);
        const auto end = positions.toQt(endByte);
        if (!start || !end) return false;
        auto next = state();
        next.text.replace(*start, *end - *start, replacement);
        next.left = left;
        next.right = right;
        return choose(next);
    }
    bool choose(const State &next)
    {
        if (pending || editor->property("inputMethodComposing").toBool() || !supported(next.text)) return false;
        if (!recovery.persist(next)) {
            status(QStringLiteral("Recovery failed; choice not applied"));
            return false;
        }
        append(next);
        render(next);
        return true;
    }
    bool move(int delta)
    {
        if (pending || editor->property("inputMethodComposing").toBool()) return false;
        const auto target = index + delta;
        if (target < 0 || target >= history.size() || !recovery.persist(history.at(target))) return false;
        index = target;
        ++acknowledged;
        render(state());
        return true;
    }
    // Explicit wrapper for paste/cut invoked from an application-owned menu.
    void nativeMethod(const char *method)
    {
        if (pending) return;
        nativeEvent = true;
        QMetaObject::invokeMethod(editor, method);
        nativeEvent = false;
        capture();
    }
    bool retry()
    {
        if (!pending || !supported(pending->text) || !recovery.persist(*pending)) return false;
        append(*pending);
        pending.reset();
        editor->setProperty("readOnly", false);
        status(QStringLiteral("Pending input recovered; unsaved draft"));
        return true;
    }
    int acknowledged = 0;
    std::optional<State> pending;
    qsizetype lastStartByte = -1;
    qsizetype lastEndByte = -1;
protected:
    bool eventFilter(QObject *target, QEvent *event) override
    {
        if (nativeEvent || rendering) return false;
        if (event->type() == QEvent::ShortcutOverride) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)) {
                event->accept();
                return true;
            }
        }
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::InputMethod) return false;
        if (pending) return true;
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->matches(QKeySequence::Undo)) { move(-1); return true; }
            if (key->matches(QKeySequence::Redo)) { move(1); return true; }
        }
        // Re-enter delivery once under a guard, then observe the complete native event.
        // An IME replacement can emit more than one internal document notification.
        nativeEvent = true;
        QGuiApplication::sendEvent(target, event);
        nativeEvent = false;
        capture();
        return true;
    }
private slots:
    void capture()
    {
        if (rendering || nativeEvent || pending || history.isEmpty()) return;
        const auto text = bufferText(document);
        const auto before = state();
        if (text == before.text) return; // Pre-edit and formatting are not commands.
        qsizetype start = 0;
        while (start < text.size() && start < before.text.size() && text.at(start) == before.text.at(start)) ++start;
        if (start > 0 && start < before.text.size() && before.text.at(start).isLowSurrogate()) --start;
        qsizetype oldEnd = before.text.size();
        qsizetype newEnd = text.size();
        while (oldEnd > start && newEnd > start && before.text.at(oldEnd - 1) == text.at(newEnd - 1)) {
            --oldEnd;
            --newEnd;
        }
        if (oldEnd < before.text.size() && before.text.at(oldEnd).isLowSurrogate()) { ++oldEnd; ++newEnd; }
        const Positions positions(before.text);
        const auto first = positions.toByte(start);
        const auto last = positions.toByte(oldEnd);
        State next = before;
        next.text = text;
        if (!first || !last || !supported(text)) {
            pending = next;
            editor->setProperty("readOnly", true);
            status(QStringLiteral("Unsupported pending text; editing blocked"));
            return;
        }
        lastStartByte = *first;
        lastEndByte = *last;
        if (!recovery.persist(next)) {
            pending = next;
            editor->setProperty("readOnly", true);
            status(QStringLiteral("Recovery failed; pending input not acknowledged"));
            return;
        }
        append(next);
    }
private:
    void status(const QString &message) { root->setProperty("statusText", message); }
    void append(const State &next)
    {
        history.resize(index + 1);
        history.append(next);
        ++index;
        ++acknowledged;
        status(QStringLiteral("Recovered command; unsaved draft"));
    }
    void render(const State &next)
    {
        rendering = true;
        editor->setProperty("text", next.text);
        rendering = false;
    }
    QQuickItem *editor;
    QQuickItem *root;
    QTextDocument *document;
    Recovery &recovery;
    QList<State> history;
    qsizetype index = 0;
    bool rendering = false;
    bool nativeEvent = false;
};

class DifferenceHighlight : public QSyntaxHighlighter {
public:
    explicit DifferenceHighlight(QTextDocument *document) : QSyntaxHighlighter(document) {}
protected:
    void highlightBlock(const QString &text) override
    {
        QTextCharFormat format;
        format.setBackground(Qt::yellow);
        format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        for (qsizetype pos = text.indexOf(QStringLiteral("repeat")); pos >= 0;
             pos = text.indexOf(QStringLiteral("repeat"), pos + 6)) {
            setFormat(static_cast<int>(pos), 6, format);
        }
    }
};
void input(QQuickItem *editor, const QString &commit, const QString &preedit = {}, int replaceStart = 0, int replaceLength = 0)
{
    QInputMethodEvent event(preedit, {});
    event.setCommitString(commit, replaceStart, replaceLength);
    QGuiApplication::sendEvent(editor, &event);
}
void key(QQuickItem *editor, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {})
{
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QGuiApplication::sendEvent(editor, &event);
}
void select(QQuickItem *editor, int start, int end)
{
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, end));
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // Acceptance fixtures own the offscreen clipboard only. Never replace a
    // user's desktop clipboard when the command is run without its documented env.
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        QTextStream(stderr) << "Run with QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software\n";
        return 2;
    }
    QQuickView view;
    view.setSource(QUrl(QStringLiteral("qrc:/Editor.qml")));
    if (view.status() != QQuickView::Ready) return 2;
    view.show();
    app.processEvents();
    auto *editor = findItem(view.rootObject(), QStringLiteral("result"));
    auto *source = findItem(view.rootObject(), QStringLiteral("sourceA"));
    if (!editor || !source) return 2;
    auto *document = documentFor(editor);
    editor->forceActiveFocus();
    if (app.arguments().contains(QStringLiteral("--baseline"))) {
        editor->setProperty("text", QString::fromUtf8("Aé😀é\n"));
        check(bufferText(document).toUtf8().size() == 11, "baseline byte count");
        check(editor->property("length").toInt() == 7, "baseline UTF-16 units");
        check(document->isUndoRedoEnabled(), "baseline native history enabled");
        editor->setProperty("text", QStringLiteral("seed"));
        editor->setProperty("cursorPosition", 4);
        input(editor, {}, QStringLiteral("に"));
        check(editor->property("inputMethodComposing").toBool(), "baseline preedit active");
        check(bufferText(document) == QStringLiteral("seed"), "baseline preedit outside committed text");
        input(editor, QStringLiteral("日本"));
        check(bufferText(document) == QStringLiteral("seed日本"), "baseline IME commit delivered");
        editor->setProperty("text", QString::fromUtf8("a b"));
        check(document->toPlainText() == QStringLiteral("a b"), "baseline toPlainText normalizes NBSP");
        check(bufferText(document) == QString::fromUtf8("a b"), "raw document preserves NBSP");
        QTextStream(stdout) << "BASELINE bytes=11 units=7 native-undo=enabled preedit=uncommitted NBSP=normalized-by-toPlainText\n";
    } else {
        QTemporaryDir fixtures(QStringLiteral("/tmp/truce-editor-fixtures-XXXXXX"));
        if (!fixtures.isValid()) return 2;
        const auto fixturePath = fixtures.path();
        {
            Recovery recovery(fixtures.filePath(QStringLiteral("snapshot.json")));
            Adapter adapter(editor, view.rootObject(), recovery);
            const QString unicode = QString::fromUtf8("Aé😀é\t\nrepeat\nrepeat\nNBSP:  ZWJ:👩‍💻 RTL:ع\n");
            recovery.failWrites = true;
            check(!adapter.initialize(unicode) && editor->property("readOnly").toBool(), "initial recovery failure blocks editing");
            recovery.failWrites = false;
            check(!adapter.initialize(QStringLiteral("CR\r\n")), "CRLF requires explicit conversion");
            check(!adapter.initialize(QString(QChar(0x2029))), "paragraph separator requires lossless projection");
            check(!adapter.initializeUtf8(QByteArray::fromHex("c080")), "malformed UTF-8 rejected");
            check(adapter.initializeUtf8(unicode.toUtf8()), "initial snapshot before enabling edits");
            check((QFile::permissions(recovery.path) & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                  | QFileDevice::ReadOther | QFileDevice::WriteOther)) == 0, "recovery payload owner-only");
            check(!document->isUndoRedoEnabled(), "native history disabled");
            check(bufferText(document) == unicode, "plain Unicode round trip including final LF");
            Positions positions(unicode);
            for (qsizetype qt = 0; qt <= unicode.size(); ++qt) {
                const auto byte = positions.toByte(qt);
                if (byte) check(positions.toQt(*byte) == qt, "every scalar boundary round trips");
            }
            for (qsizetype byte = 0; byte <= unicode.toUtf8().size(); ++byte) {
                const auto qt = positions.toQt(byte);
                if (qt) check(positions.toByte(*qt) == byte, "every byte boundary round trips");
            }
            check(positions.toByte(4) == 7, "astral character maps 4 UTF-16 units to 7 bytes");
            check(!positions.toByte(3), "reject split surrogate");
            check(!positions.toQt(4) && !positions.toQt(2), "reject interior UTF-8 bytes");
            check(!positions.toByte(-1) && !positions.toQt(999), "reject out-of-range offsets");
            check(!Positions(QString(QChar(0xD800))).valid, "reject unpaired surrogate");
            // Native committed IME input, repeated preedit, commit plus new preedit,
            // replacement and cancellation. Counts are command counts, not signals.
            editor->setProperty("cursorPosition", 4);
            const int beforeIme = adapter.acknowledged;
            input(editor, {}, QStringLiteral("に"));
            input(editor, {}, QStringLiteral("にほ"));
            check(adapter.acknowledged == beforeIme && adapter.state().text == unicode,
                  "preedit updates have no acknowledgment");
            check(!adapter.choose(State{QStringLiteral("wrong")}), "choices blocked during composition");
            input(editor, QStringLiteral("日本"), QStringLiteral("ご"));
            check(adapter.acknowledged == beforeIme + 1, "commit plus new preedit is one transaction");
            check(editor->property("inputMethodComposing").toBool(), "new preedit remains visible");
            input(editor, {});
            check(adapter.acknowledged == beforeIme + 1, "cancel preedit adds no command");
            input(editor, QStringLiteral("語"), {}, -1, 1);
            check(adapter.acknowledged == beforeIme + 2, "IME replacement is one transaction");
            check(adapter.lastStartByte == 10 && adapter.lastEndByte == 13, "IME replacement mapped in old UTF-8 bytes");
            editor->setProperty("cursorPosition", 0);
            const int beforeKey = adapter.acknowledged;
            key(editor, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            key(editor, Qt::Key_Backspace);
            check(adapter.acknowledged == beforeKey + 2, "insert and delete acknowledged independently");
            const auto beforePaste = adapter.state();
            QGuiApplication::clipboard()->setText(QStringLiteral("paste\t😀"));
            adapter.nativeMethod("paste");
            check(adapter.acknowledged == beforeKey + 3, "paste is one transaction");
            check(adapter.state().text.startsWith(QStringLiteral("paste\t😀")), "clipboard paste preserved");
            select(editor, 0, 5);
            adapter.nativeMethod("copy");
            check(QGuiApplication::clipboard()->text() == QStringLiteral("paste"), "copy uses selection");
            adapter.nativeMethod("cut");
            check(adapter.acknowledged == beforeKey + 4, "cut is one transaction");
            check(adapter.move(-1) && adapter.move(-1) && adapter.state() == beforePaste, "undo cut and paste in order");
            // Command-owned choices, Ignore without text change, manual edit, ordered combination.
            const State a{QStringLiteral("first\nrepeat\nrepeat\n"), QStringLiteral("accepted"), QStringLiteral("undecided")};
            check(adapter.choose(a), "accept first contribution");
            State ignored = a;
            ignored.right = QStringLiteral("ignored");
            check(adapter.choose(ignored), "Ignore changes decisions only");
            key(editor, Qt::Key_Z, Qt::ControlModifier);
            check(adapter.state() == a, "Ctrl-Z restores decision-only command");
            check(adapter.move(1) && adapter.state() == ignored, "redo restores Ignore");
            check(adapter.move(-1), "undo Ignore before later edit");
            editor->setProperty("cursorPosition", 5);
            key(editor, Qt::Key_Exclam, Qt::NoModifier, QStringLiteral("!"));
            const State manual = adapter.state();
            check(manual.text.startsWith(QStringLiteral("first!")) && manual.left == QStringLiteral("accepted"), "manual edit retains decisions");
            // The fixture explicitly supplies a confirmed replacement, not inferred correspondence.
            State combined = manual;
            combined.text = QStringLiteral("first!\nsecond\nrepeat\nrepeat\n");
            combined.right = QStringLiteral("accepted-after-left-confirmed");
            check(adapter.replace(adapter.acknowledged, 7, 7, QStringLiteral("second\n"),
                                  combined.left, combined.right), "confirmed second contribution after manual edit");
            check(adapter.state() == combined, "combination preserves manually edited first side");
            check(adapter.move(-1) && adapter.state() == manual, "undo combination restores text and choices");
            check(adapter.move(-1) && adapter.state() == a, "undo manual edit follows stack");
            check(adapter.move(1) && adapter.move(1) && adapter.state() == combined, "redo command order");
            // Partial line choices with unequal lengths and distinct explicit byte ranges.
            const State partial{QStringLiteral("left-long\nright\n"), QStringLiteral("line-1"), QStringLiteral("line-2")};
            check(adapter.replace(adapter.acknowledged, 0, adapter.state().text.toUtf8().size(),
                                  partial.text, partial.left, partial.right), "explicit partial line command");
            check(adapter.move(-1) && adapter.state() == combined, "partial line fixture undo");
            check(adapter.move(1) && adapter.state() == partial, "partial line fixture redo");
            // Failed history persistence must leave the redo transaction available.
            check(adapter.move(-1) && adapter.state() == combined, "undo before failed redo");
            const auto beforeRedoFailure = adapter.acknowledged;
            recovery.failWrites = true;
            check(!adapter.move(1) && adapter.state() == combined
                  && adapter.acknowledged == beforeRedoFailure, "failed redo preserves text choices and revision");
            check(recovery.restore() == combined, "failed redo preserves the durable snapshot");
            recovery.failWrites = false;
            check(adapter.move(1) && adapter.state() == partial, "redo remains available after persistence retry");
            check(adapter.move(-1) && adapter.choose(partial), "new command after undo replaces the redo branch");
            check(!adapter.move(1) && adapter.state() == partial, "discarded redo cannot replace a newer command");
            const auto beforeInvalidRange = adapter.acknowledged;
            check(!adapter.replace(beforeInvalidRange, 2, 1, QStringLiteral("wrong"), a.left, a.right),
                  "reversed byte range rejected");
            check(!adapter.choose(State{QString(QChar(0x2028))}) && adapter.state() == partial
                  && adapter.acknowledged == beforeInvalidRange, "unsupported choice leaves history and text unchanged");
            // A deleted anchor is rejected by the fixture's exact revision/range guard.
            const auto anchorRevision = adapter.acknowledged;
            const auto anchorText = adapter.state().text;
            select(editor, 0, 10);
            key(editor, Qt::Key_Backspace);
            check(adapter.acknowledged != anchorRevision && adapter.state().text != anchorText,
                  "deleted anchor changes captured revision");
            const auto afterDelete = adapter.state();
            check(!adapter.replace(anchorRevision, 10, 15, QStringLiteral("wrong"), a.left, a.right)
                  && adapter.state() == afterDelete, "stale deleted anchor command rejected without fallback");
            check(!adapter.replace(adapter.acknowledged, 0, 999, QStringLiteral("wrong"), a.left, a.right), "out-of-buffer range rejected");
            // Reading aids operate on document layout/formatting, never serialized text.
            const State reading{QStringLiteral("repeat \tword with long context repeated repeatedly for wrapping\nrepeat\n")};
            check(adapter.choose(reading), "load reading fixture as a command");
            const int beforeAids = adapter.acknowledged;
            DifferenceHighlight highlighter(document);
            highlighter.rehighlight();
            auto options = document->defaultTextOption();
            options.setFlags(options.flags() | QTextOption::ShowTabsAndSpaces);
            document->setDefaultTextOption(options);
            const qreal unwrappedHeight = editor->property("contentHeight").toReal();
            view.rootObject()->setProperty("wrapText", true);
            app.processEvents();
            check(editor->property("contentHeight").toReal() > unwrappedHeight, "wrapping changes layout height");
            const auto match = document->find(QStringLiteral("repeat"), 1);
            check(!match.isNull() && match.selectionStart() > 0, "scoped search reaches second repeated passage");
            select(editor, match.selectionStart(), match.selectionEnd());
            const auto selection = editor->property("selectionStart").toInt();
            view.rootObject()->setProperty("earlierVisible", true);
            view.rootObject()->setProperty("earlierVisible", false);
            check(editor->property("selectionStart").toInt() == selection, "base visibility preserves selection");
            check(!document->firstBlock().layout()->formats().isEmpty(), "word highlight has non-color underline");
            check(document->defaultTextOption().flags().testFlag(QTextOption::ShowTabsAndSpaces), "whitespace rendering enabled");
            check(bufferText(document) == reading.text && adapter.acknowledged == beforeAids,
                  "reading aids preserve text decisions and history");
            source->setProperty("text", reading.text);
            key(source, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            check(bufferText(documentFor(source)) == reading.text, "source remains read-only");
            // Recovery failure preserves visible pending input and the last acknowledged snapshot.
            const auto acknowledged = adapter.state();
            const int beforeFailure = adapter.acknowledged;
            recovery.failWrites = true;
            check(!adapter.choose(a) && !adapter.move(-1) && adapter.state() == acknowledged
                  && adapter.acknowledged == beforeFailure, "choice and undo persistence failure leave state unchanged");
            editor->setProperty("cursorPosition", 0);
            key(editor, Qt::Key_P, Qt::NoModifier, QStringLiteral("p"));
            check(adapter.pending.has_value() && editor->property("readOnly").toBool(), "failed persistence freezes pending buffer");
            check(view.rootObject()->property("statusText").toString().contains(QStringLiteral("not acknowledged")),
                  "pending failure visible in scene status");
            check(adapter.acknowledged == beforeFailure && adapter.state() == acknowledged, "failed persistence does not acknowledge");
            const Recovery reopened(recovery.path);
            check(reopened.restore() == acknowledged, "new reader restores last acknowledged text and decisions");
            const auto pendingText = bufferText(document);
            key(editor, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
            check(bufferText(document) == pendingText && !adapter.move(-1) && !adapter.choose(a), "failure blocks further editing and history");
            recovery.failWrites = false;
            check(adapter.retry() && adapter.acknowledged == beforeFailure + 1, "retry acknowledges pending command exactly once");
            check(recovery.restore() == adapter.state() && bufferText(document) == pendingText, "recovery state equals retried buffer");
            const auto afterRetry = adapter.acknowledged;
            check(!adapter.retry() && adapter.acknowledged == afterRetry && !adapter.pending,
                  "repeating a successful recovery retry cannot acknowledge input twice");
            Recovery unavailable(fixtures.filePath(QStringLiteral("missing/snapshot.json")));
            check(!unavailable.persist(a), "actual unavailable-path I/O fails");
            // Corruption is rejected rather than interpreted as an empty draft.
            QFile corrupt(recovery.path);
            check(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate), "open disposable corrupt fixture");
            corrupt.write("{\"version\":1,\"payload\":\"eA==\",\"sha256\":\"wrong\"}");
            corrupt.close();
            check(!recovery.restore(), "integrity mismatch rejected");
            QGuiApplication::clipboard()->clear();
            QTextStream(stdout) << "RECOVERY writes=" << recovery.writes
                                << " max-snapshot-ns=" << recovery.maxNanos
                                << " acknowledged=" << adapter.acknowledged << '\n';
        }
        check(fixtures.remove() && !QFile::exists(fixturePath), "disposable recovery fixtures removed");
        QTextStream(stdout) << "FIXTURES cleaned; clipboard cleared\n";
    }
    QTextStream(stdout) << "RESULT assertions=" << assertions << " failures=" << failures
                        << " qt=" << qVersion() << " platform=" << QGuiApplication::platformName() << '\n';
    return failures == 0 ? 0 : 1;
}
#include "main.moc"
