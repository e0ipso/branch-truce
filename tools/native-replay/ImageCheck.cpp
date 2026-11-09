// Decodes, compares and crops native replay PNGs with exact pixels: no interpolation, thresholds or masks.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <functional>
#include <optional>

namespace {
constexpr int Failure = 1;
constexpr int UsageError = 2;

const char *const Overview = R"(Usage: native-replay-image COMMAND [options] ARGUMENTS
Decodes, compares and crops native replay PNGs with exact pixels.

Commands:
  decode IMAGE                  print width, height and the SHA-256 of the decoded RGBA pixels
  compare IMAGE OTHER           also count the pixels that differ between two same-sized images
  crop IMAGE OUTPUT             save an unscaled region of IMAGE
  artifacts REFERENCE NATIVE PREFIX
                                compare two regions on a common grid and save close-ups,
                                overlay and difference images under PREFIX

Run 'native-replay-image COMMAND --help' for a command's options.
Exit status: 0 success; 1 unreadable, unwritable or out-of-bounds input; 2 usage error.
Results are compact JSON on stdout; errors go to stderr.
)";

int fail(const QString &message)
{
    std::fprintf(stderr, "native-replay-image: %s\n", qPrintable(message));
    return Failure;
}

void printJson(const QJsonObject &object)
{
    std::puts(QJsonDocument(object).toJson(QJsonDocument::Compact).constData());
}

QString describe(const QRect &rect)
{
    return QStringLiteral("%1,%2,%3,%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}

QImage decoded(const QString &path) { return QImage(path).convertToFormat(QImage::Format_RGBA8888); }

QJsonObject identity(const QImage &image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4));
    return {{"width", image.width()},
            {"height", image.height()},
            {"decoded_sha256", QString::fromLatin1(hash.result().toHex())}};
}

// One parsed subcommand. Readers record the first malformed value as a usage error.
struct Invocation {
    const QCommandLineParser &parser;
    QStringList positional;
    QString usageError;

    std::optional<QRect> rect(const QString &option)
    {
        const auto parts = parser.value(option).split(u',');
        int numbers[4] = {};
        bool ok = parts.size() == 4;
        for (qsizetype i = 0; ok && i < 4; ++i) numbers[i] = parts[i].toInt(&ok);
        if (ok) return QRect(numbers[0], numbers[1], numbers[2], numbers[3]);
        reject(QStringLiteral("--%1 must be X,Y,WIDTH,HEIGHT integers, not '%2'").arg(option, parser.value(option)));
        return std::nullopt;
    }

    std::optional<int> replication(const QString &option)
    {
        bool ok = false;
        const int value = parser.value(option).toInt(&ok);
        if (ok && value >= 1 && value <= 3) return value;
        reject(QStringLiteral("--%1 must be 1, 2 or 3, not '%2'").arg(option, parser.value(option)));
        return std::nullopt;
    }

    void reject(const QString &error)
    {
        if (usageError.isEmpty()) usageError = error;
    }
};

int decode(Invocation &in)
{
    const auto image = decoded(in.positional[0]);
    if (image.isNull()) return fail("cannot decode " + in.positional[0]);
    printJson(identity(image));
    return 0;
}

int compare(Invocation &in)
{
    const auto image = decoded(in.positional[0]), other = decoded(in.positional[1]);
    if (image.isNull() || other.isNull()) return fail("cannot decode " + in.positional[image.isNull() ? 0 : 1]);
    if (other.size() != image.size()) return fail("the images differ in size");
    qint64 different = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixel(x, y) != other.pixel(x, y)) ++different;
    auto result = identity(image);
    result["differing_pixels"] = different;
    printJson(result);
    return 0;
}

int crop(Invocation &in)
{
    const auto rect = in.rect("rect");
    if (!rect) return UsageError;
    const QImage image(in.positional[0]);
    if (image.isNull()) return fail("cannot decode " + in.positional[0]);
    if (rect->isEmpty() || !image.rect().contains(*rect))
        return fail(QStringLiteral("--rect %1 is empty or outside the image").arg(describe(*rect)));
    // Diagnostic close-ups retain exact native pixels, with no scaling or paint.
    if (!image.copy(*rect).save(in.positional[1])) return fail("cannot write " + in.positional[1]);
    return 0;
}

int artifacts(Invocation &in)
{
    const auto referenceRect = in.rect("reference-rect"), nativeRect = in.rect("native-rect");
    const auto referenceGrid = in.rect("reference-grid");
    const auto referenceReplication = in.replication("reference-replication");
    const auto nativeReplication = in.replication("native-replication");
    if (!in.usageError.isEmpty()) return UsageError;
    const QImage reference(in.positional[0]), native(in.positional[1]);
    const QString prefix = in.positional[2];
    if (reference.isNull() || native.isNull())
        return fail("cannot decode " + in.positional[reference.isNull() ? 0 : 1]);
    if (referenceRect->isEmpty() || !reference.rect().contains(*referenceRect))
        return fail(QStringLiteral("--reference-rect %1 is empty or outside the image").arg(describe(*referenceRect)));
    if (nativeRect->isEmpty() || !native.rect().contains(*nativeRect))
        return fail(QStringLiteral("--native-rect %1 is empty or outside the image").arg(describe(*nativeRect)));
    const auto originalA = reference.copy(*referenceRect).convertToFormat(QImage::Format_RGBA8888);
    const auto originalB = native.copy(*nativeRect).convertToFormat(QImage::Format_RGBA8888);
    // Exact cell replication onto the declared common grid. No interpolation,
    // thresholds or masks. Close-ups retain original decoded pixels.
    auto a = originalA.scaled(originalA.size() * *referenceReplication, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const auto b =
        originalB.scaled(originalB.size() * *nativeReplication, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (referenceGrid->isEmpty() || !a.rect().contains(*referenceGrid))
        return fail(QStringLiteral("--reference-grid %1 is empty or outside the replicated reference region")
                        .arg(describe(*referenceGrid)));
    a = a.copy(*referenceGrid);
    const QSize size(qMax(a.width(), b.width()), qMax(a.height(), b.height()));
    QImage overlay(size, QImage::Format_RGBA8888), difference(size, QImage::Format_RGBA8888);
    qint64 changed = 0, unmatched = 0;
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            const bool inA = a.rect().contains(x, y), inB = b.rect().contains(x, y);
            const QColor ca = inA ? a.pixelColor(x, y) : QColor(0, 0, 0, 0);
            const QColor cb = inB ? b.pixelColor(x, y) : QColor(0, 0, 0, 0);
            overlay.setPixelColor(x, y,
                                  QColor((ca.red() + cb.red()) / 2, (ca.green() + cb.green()) / 2,
                                         (ca.blue() + cb.blue()) / 2, qMax(ca.alpha(), cb.alpha())));
            difference.setPixelColor(
                x, y,
                QColor(qAbs(ca.red() - cb.red()), qAbs(ca.green() - cb.green()), qAbs(ca.blue() - cb.blue()), 255));
            if (inA != inB) ++unmatched;
            if (ca != cb) ++changed;
        }
    if (!originalA.save(prefix + "-reference.png") || !originalB.save(prefix + "-native.png")
        || !overlay.save(prefix + "-overlay.png") || !difference.save(prefix + "-difference.png"))
        return fail("cannot write the artifacts under " + prefix);
    printJson({{"reference_replication", *referenceReplication},
               {"native_replication", *nativeReplication},
               {"reference_common_grid_rect",
                QJsonArray{referenceGrid->x(), referenceGrid->y(), referenceGrid->width(), referenceGrid->height()}},
               {"differing_pixels", changed},
               {"unmatched_extent_pixels", unmatched},
               {"reference_top_left", a.pixelColor(0, 0).name(QColor::HexArgb)},
               {"native_top_left", b.pixelColor(0, 0).name(QColor::HexArgb)},
               {"overlay_top_left", overlay.pixelColor(0, 0).name(QColor::HexArgb)},
               {"difference_top_left", difference.pixelColor(0, 0).name(QColor::HexArgb)},
               {"overlay", prefix + "-overlay.png"},
               {"difference", prefix + "-difference.png"}});
    return 0;
}

struct Command {
    QString name;
    QString description;
    QList<QPair<QString, QString>> positional;
    QList<QCommandLineOption> options; // Every option is required.
    std::function<int(Invocation &)> run;
};

QList<Command> commands()
{
    const auto rect = [](const QString &name, const QString &description) {
        return QCommandLineOption(name, description + " as X,Y,WIDTH,HEIGHT", "rect");
    };
    const auto replication = [](const QString &name, const QString &image) {
        return QCommandLineOption(name, "cells per " + image + " pixel on the common grid: 1, 2 or 3", "n");
    };
    return {
        {"decode",
         "Print width, height and the SHA-256 of the decoded RGBA pixels.",
         {{"image", "PNG to decode"}},
         {},
         decode},
        {"compare",
         "Print decode's fields for IMAGE and the number of pixels that differ from OTHER.",
         {{"image", "PNG to decode"}, {"other", "PNG of the same size"}},
         {},
         compare},
        {"crop",
         "Save an unscaled region of IMAGE to OUTPUT.",
         {{"image", "PNG to crop"}, {"output", "PNG to write"}},
         {rect("rect", "region of IMAGE")},
         crop},
        {"artifacts",
         "Replicate a reference and a native region onto a common grid, count differing and unmatched cells, "
         "and save PREFIX-reference.png, PREFIX-native.png, PREFIX-overlay.png and PREFIX-difference.png.",
         {{"reference", "approved reference PNG"},
          {"native", "native capture PNG"},
          {"prefix", "artifact path prefix"}},
         {rect("reference-rect", "region of REFERENCE"), rect("native-rect", "region of NATIVE"),
          replication("reference-replication", "reference"), replication("native-replication", "native"),
          rect("reference-grid", "crop of the replicated reference region")},
         artifacts},
    };
}

int usage(const QString &error, const QString &help)
{
    std::fprintf(stderr, "native-replay-image: %s\n\n%s", qPrintable(error), qPrintable(help));
    return UsageError;
}

int run(const Command &command, const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(command.description);
    parser.addHelpOption();
    parser.addPositionalArgument(command.name, "the command", command.name);
    for (const auto &[name, description] : command.positional) parser.addPositionalArgument(name, description);
    parser.addOptions(command.options);
    if (!parser.parse(arguments)) return usage(parser.errorText(), parser.helpText());
    if (parser.isSet("help")) {
        std::fputs(qPrintable(parser.helpText()), stdout);
        return 0;
    }
    Invocation in{parser, parser.positionalArguments().mid(1), {}};
    if (in.positional.size() != command.positional.size())
        return usage(QStringLiteral("%1 takes %2 arguments").arg(command.name).arg(command.positional.size()),
                     parser.helpText());
    for (const auto &option : command.options)
        if (!parser.isSet(option.names().constFirst()))
            return usage("--" + option.names().constFirst() + " is required", parser.helpText());
    const int status = command.run(in);
    return status == UsageError ? usage(in.usageError, parser.helpText()) : status;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() < 2) return usage("a command is required", Overview);
    if (arguments[1] == "-h" || arguments[1] == "--help") {
        std::fputs(Overview, stdout);
        return 0;
    }
    for (const auto &command : commands())
        if (command.name == arguments[1]) return run(command, arguments);
    return usage("unknown command '" + arguments[1] + "'", Overview);
}
