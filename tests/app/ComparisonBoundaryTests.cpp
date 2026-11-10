// Backspace and Delete at the resource limits, in multi-megabyte buffers in the real editor.
#include "ComparisonScene.h"

using namespace comparison_scene;

namespace {
// atLimit ends in a non-newline byte that is exactly at a resource limit. Qt
// reports Backspace and Delete with control characters in text(), as real
// keyboards do, so the editor must not predict them as insertions.
void deletionAtLimit(const std::string &atLimit, const QString &rejection)
{
    Fixture fixture(test_support::choiceFile("x\n", "x\n", atLimit));
    auto *editor = fixture.item("resultText");
    const auto before = fixture.file().current();
    const auto size = static_cast<int>(atLimit.size());
    CHECK(fixture.text().toStdString() == atLimit);
    CHECK(!editor->property("readOnly").toBool() && fixture.controller.feedback()->message().isEmpty());

    fixture.select(1, 1);
    fixture.key(Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    CHECK(fixture.file().current().id == before.id && fixture.text().toStdString() == atLimit);
    CHECK(!editor->property("readOnly").toBool());
    CHECK(fixture.controller.feedback()->message() == rejection);
    fixture.checkRecovered(before.draft);

    fixture.select(size, size);
    fixture.key(Qt::Key_Backspace, Qt::NoModifier, QStringLiteral("\b"));
    const auto backspaced = fixture.file().current();
    CHECK(backspaced.id == before.id + 1);
    CHECK(backspaced.draft.text == atLimit.substr(0, atLimit.size() - 1));
    CHECK(fixture.text().toStdString() == backspaced.draft.text);
    CHECK(fixture.controller.feedback()->message().isEmpty());
    fixture.checkRecovered(backspaced.draft);

    fixture.select(size - 1, size - 1);
    fixture.key(Qt::Key_A, Qt::NoModifier, QString(QLatin1Char(atLimit.back())));
    const auto refilled = fixture.file().current();
    CHECK(refilled.id == backspaced.id + 1 && refilled.draft.text == atLimit);

    fixture.select(0, 0);
    fixture.key(Qt::Key_Delete, Qt::NoModifier, QStringLiteral("\x7f"));
    const auto deleted = fixture.file().current();
    CHECK(deleted.id == refilled.id + 1);
    CHECK(deleted.draft.text == atLimit.substr(1));
    CHECK(fixture.text().toStdString() == deleted.draft.text);
    CHECK(!editor->property("readOnly").toBool() && fixture.controller.feedback()->message().isEmpty());
    fixture.checkRecovered(deleted.draft);
}
}

void comparison_scene::addBoundaryCases(Groups groups)
{
    groups.boundary.run("Backspace and Delete shorten an exact 262144-byte line", [] {
        deletionAtLimit(std::string(262144, 'a'),
                        QStringLiteral("Line of 262145 bytes exceeds the supported 262144 byte line limit. "
                                       "The existing edit was kept and this change was not acknowledged."));
    });
    groups.boundary.run("Backspace and Delete shorten an exact 8388608-byte buffer", [] {
        // Lines stay below the line limit so only the buffer limit applies.
        std::string atLimit;
        atLimit.reserve(8388608);
        for (int i = 0; i < 63; ++i) atLimit += std::string(131071, 'p') + '\n';
        atLimit += std::string(131072, 'p');
        CHECK(atLimit.size() == 8388608 && assessText(atLimit).limit == TextLimit::Within);
        deletionAtLimit(
            atLimit,
            QStringLiteral("Text of 8388609 bytes exceeds the supported 8388608 byte recovery payload limit. "
                           "The existing edit was kept and this change was not acknowledged."));
    });
}
