// Compare accepted controller edits with an active QML selected-file consumer.
#include "app/ApplicationController.h"
#include "fixtures/ChoiceFile.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

using namespace branch_truce;

// Same line generator and variant scheme as WorkloadMeasurements.cpp.
static std::string makeFile(std::size_t lines, int variant)
{
    std::string out;
    for (std::size_t i = 0; i < lines; ++i) {
        if (i % 40 == 0) { out += "repeat-passage\n"; continue; }
        char prefix[32];
        const int count = std::snprintf(prefix, sizeof prefix, "L%06zu-", i);
        std::string line(prefix, static_cast<std::size_t>(count));
        if (i % 100 == 7) line += "caf\xc3\xa9-";
        const bool changed = variant >= 0 && i % 25 == static_cast<std::size_t>(variant);
        line.append(94 - line.size(), 'a');
        line.push_back(changed ? 'Z' : 'a');
        line.push_back('\n');
        out += line;
    }
    return out;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto left = makeFile(4000, 0);
    const auto right = makeFile(4000, 1);
    const auto initial = makeFile(4000, 2);
    ApplicationController controller("/usr/bin/git");
    if (!controller.files()->appendFile(test_support::choiceFile(left, right, initial))
        || !controller.selectFile("choices")) return 1;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("controller", &controller);
    QQmlComponent component(&engine);
    component.setData(R"(import QtQml
        QtObject {
            property var file: controller.selectedFile
            property string result: file.resultText
            property string source: file.stage2.text
            property int passages: file.passages.length
            property string revision: file.currentRevision
        })", QUrl());
    std::unique_ptr<QObject> consumer(component.create());
    if (!consumer) { std::cerr << component.errorString().toStdString(); return 2; }
    if (consumer->property("result").toString().toUtf8().toStdString() != initial
        || consumer->property("source").toString().toUtf8().toStdString() != left) return 3;
    std::string expected = initial;
    std::cout << "bytes=" << initial.size() << " lines=4000 samples=10 build=Debug\n";
    for (int i = 0; i < 10; ++i) {
        const auto revision = consumer->property("revision").toString();
        QElapsedTimer timer;
        timer.start();
        const bool accepted = controller.editResult("choices", revision, 0, 0, "x");
        QCoreApplication::processEvents();
        const auto ns = timer.nsecsElapsed();
        expected.insert(0, "x");
        const auto text = consumer->property("result").toString().toUtf8().toStdString();
        if (!accepted || text != expected || controller.files()->file("choices")->current().draft.text != expected
            || consumer->property("revision").toString() == revision
            || consumer->property("source").toString().toUtf8().toStdString() != left
            || consumer->property("passages").toInt() <= 0) return 4;
        std::cout << "sample=" << i + 1 << " ms=" << static_cast<double>(ns) / 1000000.0 << '\n';
    }
}
