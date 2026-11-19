#pragma once

#include "ComparisonScene.h"

namespace comparison_scene {
// The navigation position is a footer item with the footer's de-emphasized style, shown only with a target.
inline void expectFooterPosition(Fixture &fixture, const QString &label)
{
    auto *position = fixture.item("navigationTarget");
    auto *footer = fixture.item("sessionFooter");
    CHECK(footer->isAncestorOf(position));
    CHECK(position->property("text").toString() == label);
    CHECK(position->isVisible() == !label.isEmpty());
    auto *facts = fixture.item("fileFacts");
    CHECK(position->property("font") == facts->property("font"));
    CHECK(position->property("color") == facts->property("color"));
}

inline void expectNavigation(Fixture &fixture, const FileNavigator &navigator, const Selection &selection,
                             const QString &mode, const QString &fileId, const QString &scope, int index, int count,
                             int begin, int end, const QString &label, const QString &contribution = {})
{
    const auto target = navigator.target();
    const auto actual = target.value(QStringLiteral("label")).toString();
    CHECK_EQ(actual, label);
    expectFooterPosition(fixture, label);
    CHECK(target.value(QStringLiteral("mode")).toString() == mode);
    CHECK(target.value(QStringLiteral("fileId")).toString() == fileId);
    CHECK(target.value(QStringLiteral("scope")).toString() == scope);
    CHECK(target.value(QStringLiteral("index")).toInt() == index);
    CHECK(target.value(QStringLiteral("count")).toInt() == count);
    CHECK(selection.fileId() == fileId);
    if (scope != QStringLiteral("result")) return;
    CHECK(target.value(QStringLiteral("utf16Begin")).toInt() == begin);
    CHECK(target.value(QStringLiteral("utf16End")).toInt() == end);
    CHECK(target.value(QStringLiteral("byteBegin")).toULongLong() == static_cast<qulonglong>(begin));
    CHECK(target.value(QStringLiteral("byteEnd")).toULongLong() == static_cast<qulonglong>(end));
    if (!contribution.isEmpty()) CHECK(target.value(QStringLiteral("contributionId")).toString() == contribution);
    auto *editor = fixture.item("resultText");
    const int selectionStart = editor->property("selectionStart").toInt();
    const int selectionEnd = editor->property("selectionEnd").toInt();
    CHECK_EQ(selectionStart, begin);
    CHECK_EQ(selectionEnd, end);
    CHECK(editor->property("selectedText").toString() == fixture.text().mid(begin, end - begin));
}

} // namespace comparison_scene
