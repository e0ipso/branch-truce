#!/bin/bash
set -eu
scratch=$(mktemp -d /tmp/kirigami-native-XXXXXX)
trap 'rm -rf -- "$scratch"; if [ ! -e "$scratch" ]; then printf "fixture cleanup confirmed: %s\n" "$scratch"; else exit 1; fi' EXIT
cat > "$scratch/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(kirigami_repro LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Quick Qml)
add_executable(repro main.cpp)
target_link_libraries(repro PRIVATE Qt6::Quick Qt6::Qml)
CMAKE
cat > "$scratch/main.cpp" <<'CPP'
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QCoreApplication>
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    for (int i = 0; i < 10; ++i) {
        QQmlApplicationEngine engine;
        engine.loadData(R"(
import QtQuick
import org.kde.kirigami as Kirigami
Kirigami.ApplicationWindow {
  width: 640; height: 480; visible: true
  pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.ToolBar
  pageStack.initialPage: Kirigami.Page { title: "Example" }
})");
        if (engine.rootObjects().isEmpty()) return 1;
        QCoreApplication::processEvents();
    }
}
CPP
cmake -S "$scratch" -B "$scratch/build" >/dev/null
cmake --build "$scratch/build" >/dev/null
QT_QPA_PLATFORM=offscreen QT_QUICK_CONTROLS_STYLE=org.kde.desktop "$scratch/build/repro"
printf "10 engine create/teardown cycles completed\n"
