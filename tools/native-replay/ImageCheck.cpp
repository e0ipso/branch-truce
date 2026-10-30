#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
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
