#pragma once

#include "TestHarness.h"
#include <QQmlEngine>
#include <QQmlError>

namespace test_support {
// Connect before loading. Only engine evaluation diagnostics arrive here, not platform log noise.
// A callback records failure for the next bounded wait or case boundary, never throws through Qt.
inline void failOnQmlWarnings(QQmlEngine &engine)
{
    QObject::connect(
        &engine, &QQmlEngine::warnings, &engine, [failed = false](const QList<QQmlError> &warnings) mutable {
            if (failed || warnings.isEmpty()) return;
            failed = true;
            contain([&] { recordFailure("unexpected QML warning: " + warnings.front().toString().toStdString()); });
        });
}
} // namespace test_support
