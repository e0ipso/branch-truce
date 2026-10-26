#include "TestHarness.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <utility>

namespace {
void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
}

// Symlinks every entry of source into target except the path in hidden, whose
// parent directories become real directories so their other entries stay visible.
void mirrorWithout(const QString &source, const QString &target, const QStringList &hidden)
{
    CHECK(QDir().mkpath(target));
    const auto entries = QDir(source).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto &name : entries) {
        if (!hidden.isEmpty() && name == hidden.front()) {
            if (hidden.size() > 1) mirrorWithout(source + '/' + name, target + '/' + name, hidden.mid(1));
            continue;
        }
        CHECK(QFile::link(source + '/' + name, target + '/' + name));
    }
}

// The real binary started offscreen with no Plasma session variables and the
// appearance probe preloaded. The repository argument is not a repository: the
// window and style do not depend on a session.
class Launch {
public:
    explicit Launch(const QString &probe)
    {
        CHECK(home.isValid() && runtime.isValid());
        CHECK(QFile::setPermissions(runtime.path(),
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("HOME", home.path());
        environment.insert("XDG_CONFIG_HOME", home.path() + "/config");
        environment.insert("XDG_CACHE_HOME", home.path() + "/cache");
        environment.insert("XDG_DATA_HOME", home.path() + "/data");
        environment.insert("XDG_RUNTIME_DIR", runtime.path());
        environment.insert("LC_ALL", "C");
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_QUICK_BACKEND", "software");
        environment.insert("LD_PRELOAD", probe);
    }
    QJsonObject appearance(const QString &application) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(home.path());
        process.start(application, {QStringLiteral("--smoke-test"), home.path()});
        CHECK(process.waitForStarted(5000));
        if (!process.waitForFinished(20000)) {
            process.kill();
            process.waitForFinished(5000);
            throw std::runtime_error("startup timed out: " + process.readAllStandardError().toStdString());
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            throw std::runtime_error("startup: " + process.readAllStandardError().toStdString());
        QByteArray record;
        int records = 0;
        for (const auto &line : process.readAllStandardOutput().split('\n')) {
            if (line.startsWith("BRANCH_TRUCE_APPEARANCE=")) { record = line.mid(24); ++records; }
        }
        CHECK(records == 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(record, &error);
        CHECK(error.error == QJsonParseError::NoError && document.isObject());
        return document.object();
    }
    QTemporaryDir home;
    // A short base keeps the session socket path under its length limit.
    QTemporaryDir runtime{QStringLiteral("/tmp/bt-run-XXXXXX")};
    QProcessEnvironment environment;
};

// A copy of the binary beside a qt.conf whose QML import directory mirrors the
// installed one without org/kde/desktop. Qt reads qt.conf from the binary's
// directory, and an unprivileged test cannot hide the installed module otherwise.
class HiddenKdeStyle {
public:
    explicit HiddenKdeStyle(const QString &application)
    {
        CHECK(directory.isValid());
        const auto qml = QLibraryInfo::path(QLibraryInfo::QmlImportsPath);
        CHECK(QFileInfo::exists(qml + "/org/kde/desktop/qmldir"));
        mirrorWithout(qml, directory.path() + "/qml", {"org", "kde", "desktop"});
        CHECK(!QFileInfo::exists(directory.path() + "/qml/org/kde/desktop"));
        CHECK(QFileInfo::exists(directory.path() + "/qml/org/kde/kirigami/qmldir"));
        binary = directory.path() + "/branch-truce";
        CHECK(QFile::copy(application, binary));
        const std::pair<const char *, QLibraryInfo::LibraryPath> paths[] = {
            {"Prefix", QLibraryInfo::PrefixPath}, {"Libraries", QLibraryInfo::LibrariesPath},
            {"LibraryExecutables", QLibraryInfo::LibraryExecutablesPath}, {"Binaries", QLibraryInfo::BinariesPath},
            {"Plugins", QLibraryInfo::PluginsPath}, {"ArchData", QLibraryInfo::ArchDataPath},
            {"Data", QLibraryInfo::DataPath}, {"Translations", QLibraryInfo::TranslationsPath}};
        QByteArray conf = "[Paths]\n";
        for (const auto &[key, path] : paths) conf += QByteArray(key) + '=' + QLibraryInfo::path(path).toUtf8() + '\n';
        conf += "QmlImports=" + (directory.path() + "/qml").toUtf8() + '\n';
        writeFile(directory.path() + "/qt.conf", conf);
    }
    QTemporaryDir directory;
    QString binary;
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 3) return 2;
    const auto binary = application.arguments()[1];
    const auto probe = application.arguments()[2];
    test_support::Suite suite("startup-appearance");
    suite.run("TRUCE-0103 off Plasma the installed KDE style is selected", [&] {
        const auto state = Launch(probe).appearance(binary);
        CHECK(state.value("style") == "org.kde.desktop");
        CHECK(state.value("displayName") == "Branch Truce");
    });
    suite.run("TRUCE-0103 an explicit QT_QUICK_CONTROLS_STYLE overrides the KDE style", [&] {
        Launch launch(probe);
        launch.environment.insert("QT_QUICK_CONTROLS_STYLE", "Basic");
        CHECK(launch.appearance(binary).value("style") == "Basic");
    });
    suite.run("TRUCE-0103 without the KDE style module the style is Fusion", [&] {
        HiddenKdeStyle hidden(binary);
        CHECK(Launch(probe).appearance(hidden.binary).value("style") == "Fusion");
    });
    suite.run("TRUCE-0103 the KDE style module on QML_IMPORT_PATH is selected", [&] {
        // Guards the hidden fixture itself: the same copy picks the module up again.
        HiddenKdeStyle hidden(binary);
        const auto extra = hidden.directory.path() + "/extra";
        CHECK(QDir().mkpath(extra + "/org/kde"));
        CHECK(QFile::link(QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + "/org/kde/desktop",
                          extra + "/org/kde/desktop"));
        Launch launch(probe);
        launch.environment.insert("QML_IMPORT_PATH", extra);
        CHECK(launch.appearance(hidden.binary).value("style") == "org.kde.desktop");
    });
    suite.run("TRUCE-0103 in a GNOME session Breeze action icons resolve through the fallback theme", [&] {
        // A display manager names the desktop, so Kirigami keeps the app's icon
        // theme and only adds its own fallback path.
        Launch launch(probe);
        launch.environment.insert("XDG_CURRENT_DESKTOP", "GNOME");
        const auto state = launch.appearance(binary);
        // The theme itself is not Breeze here, so only the fallback can supply the icon.
        CHECK(state.value("theme").toString() != "breeze");
        CHECK(state.value("fallbackTheme") == "breeze");
        CHECK(state.value("documentSave").toBool());
    });
    suite.run("with no desktop named Breeze action icons still resolve through the fallback theme", [&] {
        // Offscreen names no icon theme, which is when Kirigami swaps in its own
        // theme and search path unless XDG_CURRENT_DESKTOP is set.
        Launch launch(probe);
        CHECK(!launch.environment.contains("XDG_CURRENT_DESKTOP"));
        const auto state = launch.appearance(binary);
        CHECK(state.value("documentSave").toBool());
        CHECK(state.value("theme") == "hicolor");
        CHECK(state.value("fallbackTheme") == "breeze");
    });
    return suite.report();
}
