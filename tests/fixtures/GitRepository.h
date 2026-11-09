#pragma once

// Disposable Git repositories for the Qt suites. Fixture Git never reads the invoking user's
// configuration or repositories: HOME, the XDG config directory and the global configuration live
// in a private directory, the system configuration is off and nothing can prompt.
//
//   GitHome        the private home and global configuration, and the environment that uses them.
//   GitRepository  an empty repository on branch main, with git() to drive it.
//   OfflineGit     a `git` that refuses every network command and logs the rest, for code under
//                  test to run instead of the fixture Git.

#include "TestHarness.h"
#include "fixtures/Files.h"
#include "fixtures/FixtureGit.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <stdexcept>
#include <string>

namespace test_support {

// The Git the fixtures run, as fixtureGit() chose it.
inline QString gitExecutable() { return QFile::decodeName(QByteArray::fromStdString(fixtureGit())); }

// Runs fixture Git in `directory` and returns its standard output. An exit status other than
// `expectedExit` fails the case with Git's standard error.
inline QByteArray runGit(const QProcessEnvironment &environment, const QString &directory, const QStringList &arguments,
                         const QByteArray &input = {}, int expectedExit = 0)
{
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(gitExecutable(), QStringList{"--no-pager", "--literal-pathspecs"} + arguments);
    CHECK(process.waitForStarted(5000));
    CHECK(process.write(input) == input.size());
    process.closeWriteChannel();
    CHECK(process.waitForFinished(15000));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expectedExit)
        throw Failure("fixture git " + arguments.join(QLatin1Char(' ')).toStdString() + " exited "
                      + std::to_string(process.exitCode()) + ": " + process.readAllStandardError().toStdString());
    return process.readAllStandardOutput();
}

class GitHome {
public:
    GitHome()
    {
        CHECK(directory_.isValid());
        CHECK(QDir().mkpath(path() + QStringLiteral("/.config")));
        writeFile(globalConfig(), "[user]\n\tname = Branch Truce Tests\n\temail = tests@example.invalid\n");
        for (const auto &[name, value] : isolatedGitVariables(QFile::encodeName(path()).toStdString(),
                                                              QFile::encodeName(globalConfig()).toStdString()))
            environment_.insert(QString::fromStdString(name), QFile::decodeName(QByteArray::fromStdString(value)));
        // A fixture started by fixture Git, such as a mergetool command, runs the same Git.
        environment_.insert(QStringLiteral("BRANCH_TRUCE_TEST_GIT"), gitExecutable());
    }
    QString path() const { return directory_.path(); }
    // The only global configuration fixture Git reads. `git config --global` writes here.
    QString globalConfig() const { return path() + QStringLiteral("/.gitconfig"); }
    // The environment for fixture Git. Add to it for a child that needs more.
    const QProcessEnvironment &environment() const { return environment_; }

private:
    QTemporaryDir directory_;
    QProcessEnvironment environment_;
};

class GitRepository {
public:
    GitRepository()
    {
        CHECK(directory_.isValid());
        root_ = QFileInfo(directory_.path()).canonicalFilePath();
        git({"init", "--quiet", "--initial-branch=main"});
    }
    ~GitRepository()
    {
        // QTemporaryDir's QString traversal cannot remove non-UTF-8 names.
        for (const auto &file : rawFiles_) ::unlink(file.constData());
    }
    GitRepository(const GitRepository &) = delete;
    GitRepository &operator=(const GitRepository &) = delete;

    // The worktree, with symbolic links resolved, as Git reports it.
    QString root() const { return root_; }
    QString path(const QString &relative) const { return root_ + QLatin1Char('/') + relative; }
    const GitHome &home() const { return home_; }
    const QProcessEnvironment &environment() const { return home_.environment(); }

    QByteArray git(const QStringList &arguments, const QByteArray &input = {}, int expectedExit = 0) const
    {
        return runGit(environment(), root_, arguments, input, expectedExit);
    }
    // The index with mode, object id and stage for every entry. It neither refreshes nor writes it.
    QByteArray index() const { return git({"ls-files", "--stage", "-z"}); }

    // Writes a worktree file named by raw bytes, which need not be UTF-8. The repository removes
    // it on destruction, because QTemporaryDir cannot.
    void writePath(const QByteArray &relative, const QByteArray &bytes)
    {
        const auto file = QFile::encodeName(root_) + '/' + relative;
        if (!rawFiles_.contains(file)) rawFiles_.append(file);
        writeRawFile(file, bytes);
    }
    // Records a raw-named entry a case created some other way, such as a symbolic link.
    void adoptRawPath(const QByteArray &native) { rawFiles_.append(native); }
    // Removes the worktree and everything in it now. Returns whether it is gone.
    bool remove()
    {
        for (const auto &file : rawFiles_) ::unlink(file.constData());
        rawFiles_.clear();
        return directory_.remove() && !QFileInfo::exists(root_);
    }

private:
    GitHome home_;
    QTemporaryDir directory_;
    QString root_;
    QList<QByteArray> rawFiles_;
};

class OfflineGit {
public:
    // Every argument is logged, one per line. A network command or credential lookup is refused
    // with exit 97 and leaves a marker, so a case can assert that nothing tried to go online.
    OfflineGit()
    {
        CHECK(directory_.isValid());
        CHECK(QDir().mkpath(directory()));
        CHECK(QFile::setPermissions(directory(),
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto script = QStringLiteral("#!/bin/sh\n"
                                           "for argument in \"$@\"; do\n"
                                           "  case \"$argument\" in\n"
                                           "    fetch|pull|push|clone|ls-remote|remote|submodule|credential)\n"
                                           "      : > %1; exit 97 ;;\n"
                                           "  esac\n"
                                           "done\n"
                                           "printf '%s\\n' \"$@\" >> %2\n"
                                           "exec %3 \"$@\"\n")
                                .arg(shellQuote(networkMarker()), shellQuote(callLog()), shellQuote(gitExecutable()));
        writeFile(executable(), script.toUtf8(), true);
    }
    // The directory holding the guard, to put first on a child's PATH.
    QString directory() const { return directory_.path() + QStringLiteral("/bin"); }
    // The guard, to pass to the code under test as its Git.
    QString executable() const { return directory() + QStringLiteral("/git"); }
    bool networkAttempted() const { return QFile::exists(networkMarker()); }
    // The arguments of every call that ran, one per line.
    QByteArray calls() const { return QFile::exists(callLog()) ? readFile(callLog()) : QByteArray(); }

private:
    QString networkMarker() const { return directory_.path() + QStringLiteral("/network-attempt"); }
    QString callLog() const { return directory_.path() + QStringLiteral("/calls"); }
    QTemporaryDir directory_;
};

} // namespace test_support
