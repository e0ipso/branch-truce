#pragma once

#include "fixtures/QuickScene.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>

namespace test_support {
// Optional CI originals of the actual offscreen Qt scene, without a reference
// image or a claim of physical Plasma/AT-SPI observation.
inline void retainUiEvidence(QQuickWindow *window, const QString &name, QJsonObject metadata = {})
{
    const auto directory = qEnvironmentVariable("BRANCH_TRUCE_UI_EVIDENCE_DIR");
    if (directory.isEmpty()) return;
    CHECK(!name.isEmpty() && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1String("..")));
    waitForFrames(window, "retained UI scene " + name.toStdString());
    const auto original = window->grabWindow();
    CHECK(!original.isNull());
    CHECK(QDir().mkpath(directory));
    CHECK(original.save(QDir(directory).filePath(name + QStringLiteral(".png")), "PNG"));
    metadata.insert(QStringLiteral("schema"), 1);
    metadata.insert(
        QStringLiteral("observation"),
        QStringLiteral(
            "actual offscreen Qt scene with runtime controls style; not a physical Plasma or AT-SPI capture"));
    metadata.insert(QStringLiteral("platform"), QGuiApplication::platformName());
    metadata.insert(QStringLiteral("controls_style"), QQuickStyle::name());
    metadata.insert(QStringLiteral("source_revision"),
                    qEnvironmentVariable("BRANCH_TRUCE_CI_SOURCE_REVISION", "unavailable"));
    metadata.insert(QStringLiteral("logical_width"), window->width());
    metadata.insert(QStringLiteral("logical_height"), window->height());
    metadata.insert(QStringLiteral("image_width"), original.width());
    metadata.insert(QStringLiteral("image_height"), original.height());
    metadata.insert(QStringLiteral("reference_image"), QJsonValue::Null);
    QFile provenance(QDir(directory).filePath(name + QStringLiteral(".json")));
    CHECK(provenance.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
    CHECK(provenance.write(bytes) == bytes.size());
    CHECK(provenance.flush());
}

} // namespace test_support
