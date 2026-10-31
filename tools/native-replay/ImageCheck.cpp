#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 8 && QByteArray(argv[1]) == "--crop") {
        const QImage image(QString::fromLocal8Bit(argv[2]));
        int coordinates[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            coordinates[i] = QString::fromLocal8Bit(argv[4 + i]).toInt(&ok);
            if (!ok) return 2;
        }
        const QRect rect(coordinates[0], coordinates[1], coordinates[2], coordinates[3]);
        if (image.isNull() || rect.isEmpty() || !image.rect().contains(rect)) return 2;
        // Diagnostic close-ups retain exact native pixels, with no scaling or paint.
        return image.copy(rect).save(QString::fromLocal8Bit(argv[3])) ? 0 : 2;
    }
    if ((argc == 13 || argc == 15 || argc == 19) && QByteArray(argv[1]) == "--artifacts") {
        const QImage reference(QString::fromLocal8Bit(argv[2])), native(QString::fromLocal8Bit(argv[3]));
        const QString prefix = QString::fromLocal8Bit(argv[4]);
        QRect rects[2];
        for (int i = 0; i < 2; ++i) {
            int coordinates[4];
            for (int j = 0; j < 4; ++j) {
                bool ok = false;
                coordinates[j] = QString::fromLocal8Bit(argv[5+i*4+j]).toInt(&ok);
                if (!ok) return 2;
            }
            rects[i] = QRect(coordinates[0], coordinates[1], coordinates[2], coordinates[3]);
        }
        if (reference.isNull() || native.isNull() || rects[0].isEmpty() || rects[1].isEmpty()
            || !reference.rect().contains(rects[0]) || !native.rect().contains(rects[1])) return 2;
        const auto originalA = reference.copy(rects[0]).convertToFormat(QImage::Format_RGBA8888);
        const auto originalB = native.copy(rects[1]).convertToFormat(QImage::Format_RGBA8888);
        int replication[2] = {1, 1};
        if (argc >= 15) for (int i = 0; i < 2; ++i) {
            bool ok = false;
            replication[i] = QString::fromLocal8Bit(argv[13+i]).toInt(&ok);
            if (!ok || replication[i] < 1 || replication[i] > 3) return 2;
        }
        // Exact cell replication onto the declared common grid. No interpolation,
        // thresholds or masks. Close-ups retain original decoded pixels.
        auto a = originalA.scaled(originalA.size()*replication[0], Qt::IgnoreAspectRatio, Qt::FastTransformation);
        const auto b = originalB.scaled(originalB.size()*replication[1], Qt::IgnoreAspectRatio, Qt::FastTransformation);
        QRect referenceGrid = a.rect();
        if (argc == 19) {
            int coordinates[4];
            for (int i = 0; i < 4; ++i) {
                bool ok = false;
                coordinates[i] = QString::fromLocal8Bit(argv[15+i]).toInt(&ok);
                if (!ok) return 2;
            }
            referenceGrid = QRect(coordinates[0], coordinates[1], coordinates[2], coordinates[3]);
            if (referenceGrid.isEmpty() || !a.rect().contains(referenceGrid)) return 2;
            a = a.copy(referenceGrid);
        }
        const QSize size(qMax(a.width(), b.width()), qMax(a.height(), b.height()));
        QImage overlay(size, QImage::Format_RGBA8888), difference(size, QImage::Format_RGBA8888);
        qint64 changed = 0, unmatched = 0;
        for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
            const bool inA = a.rect().contains(x, y), inB = b.rect().contains(x, y);
            const QColor ca = inA ? a.pixelColor(x,y) : QColor(0,0,0,0);
            const QColor cb = inB ? b.pixelColor(x,y) : QColor(0,0,0,0);
            overlay.setPixelColor(x,y,QColor((ca.red()+cb.red())/2, (ca.green()+cb.green())/2,
                                             (ca.blue()+cb.blue())/2, qMax(ca.alpha(), cb.alpha())));
            difference.setPixelColor(x,y,QColor(qAbs(ca.red()-cb.red()), qAbs(ca.green()-cb.green()),
                                                qAbs(ca.blue()-cb.blue()), 255));
            if (inA != inB) ++unmatched;
            if (ca != cb) ++changed;
        }
        if (!originalA.save(prefix+"-reference.png") || !originalB.save(prefix+"-native.png")
            || !overlay.save(prefix+"-overlay.png") || !difference.save(prefix+"-difference.png")) return 2;
        const QJsonObject result{{"reference_replication", replication[0]}, {"native_replication", replication[1]},
                                 {"reference_common_grid_rect", QJsonArray{referenceGrid.x(), referenceGrid.y(), referenceGrid.width(), referenceGrid.height()}},
                                 {"differing_pixels", changed}, {"unmatched_extent_pixels", unmatched},
                                 {"reference_top_left", a.pixelColor(0,0).name(QColor::HexArgb)},
                                 {"native_top_left", b.pixelColor(0,0).name(QColor::HexArgb)},
                                 {"overlay_top_left", overlay.pixelColor(0,0).name(QColor::HexArgb)},
                                 {"difference_top_left", difference.pixelColor(0,0).name(QColor::HexArgb)},
                                 {"overlay", prefix+"-overlay.png"}, {"difference", prefix+"-difference.png"}};
        std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    if (argc != 2 && argc != 3) return 2;
    const auto image = QImage(QString::fromLocal8Bit(argv[1])).convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) return 2;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(y)), image.width()*4));
    QJsonObject result{{"width", image.width()}, {"height", image.height()},
                       {"decoded_sha256", QString::fromLatin1(hash.result().toHex())}};
    if (argc == 3) {
        const auto other = QImage(QString::fromLocal8Bit(argv[2])).convertToFormat(QImage::Format_RGBA8888);
        if (other.size() != image.size() || other.isNull()) return 2;
        qint64 different = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixel(x, y) != other.pixel(x, y)) ++different;
        result["differing_pixels"] = different;
    }
    const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::puts(json.constData());
}
