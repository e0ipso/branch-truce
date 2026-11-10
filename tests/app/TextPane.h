// A QML TextEdit with a TextBlocks over its document, for the suites that lay text out without a
// window: text blocks and connector bands.
#pragma once

#include "TestHarness.h"
#include "ui/TextBlocks.h"
#include <QAbstractTextDocumentLayout>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QRectF>
#include <QTextDocument>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace text_pane {
using branch_truce::TextBlocks;

// A TextEdit holding text, with the lines listed in folded hidden.
class Pane {
public:
    Pane(const QString &text, const QList<int> &folded)
    {
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { width: 400; tabStopDistance: 40 }",
                          QUrl(QStringLiteral("qrc:/TextBlocksTests/Pane.qml")));
        editor.reset(qobject_cast<QQuickItem *>(component.create()));
        if (!editor) throw std::runtime_error(component.errorString().toStdString());
        CHECK(editor->setProperty("text", text));
        auto *document = editor->property("textDocument").value<QQuickTextDocument *>();
        CHECK(document != nullptr);
        blocks.setDocument(document);
        QVariantList hidden;
        const auto lines = text.split(QLatin1Char('\n'));
        for (const int line : folded)
            hidden.append(QVariantMap{{QStringLiteral("line"), line}, {QStringLiteral("text"), lines.at(line - 1)}});
        blocks.setHidden(hidden);
        // Lay out every block, as showing the text would.
        auto *laidOut = document->textDocument();
        (void)laidOut->documentLayout()->blockBoundingRect(laidOut->lastBlock());
    }

    // The editor's cursor rectangle at position, which the old marker scan used.
    QRectF cursorAt(int position) const
    {
        QRectF rect;
        CHECK(QMetaObject::invokeMethod(editor.get(), "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                        Q_ARG(int, position)));
        return rect;
    }

    QQmlEngine engine;
    std::unique_ptr<QQuickItem> editor;
    TextBlocks blocks;
};

inline bool near(qreal value, qreal expected) { return std::abs(value - expected) < 1e-3; }
} // namespace text_pane
